#include "PaletteController.hpp"

#include "CurrentRowBulletDelegate.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMenu>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionComboBox>

#include <algorithm>
#include <exception>
#include <utility>

namespace amrvis::qt {

namespace {

// Selector item data: a builtin's index is >= 0; these two mark the entries
// that are not palettes.
constexpr int customEntry = -2;
constexpr int reverseToggle = -3;

} // namespace

PaletteController::PaletteController(QObject* parent)
    : QObject(parent)
{
}

QMenu* PaletteController::createMenu(QWidget* parent)
{
    auto* menu = new QMenu(tr("&Palette"), parent);
    m_menuGroup = new QActionGroup(menu);
    // A file palette leaves no builtin checked. ExclusiveOptional is the policy
    // that documents an all-unchecked group (Exclusive only happens to allow
    // it programmatically); a user click on the checked action still lands in
    // selectBuiltin, which re-checks it.
    m_menuGroup->setExclusionPolicy(
        QActionGroup::ExclusionPolicy::ExclusiveOptional);
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        auto* action = new QAction(builtinPaletteLabel(index), menu);
        action->setCheckable(true);
        action->setActionGroup(m_menuGroup);
        connect(action, &QAction::triggered, this,
            [this, index] { selectBuiltin(static_cast<int>(index)); });
        menu->addAction(action);
    }
    menu->addSeparator();
    // Reverses the selected palette's color ramp (the "_r" variant, e.g.
    // plasma_r), applied on top of whichever builtin or file palette is active.
    m_reverseAction = menu->addAction(tr("&Reverse Colormap"));
    m_reverseAction->setCheckable(true);
    m_reverseAction->setChecked(m_state.reversed);
    connect(m_reverseAction, &QAction::toggled, this,
        [this](bool on) { setReversed(on); });
    menu->addSeparator();
    auto* loadAction = menu->addAction(tr("&Load Palette File..."));
    connect(loadAction, &QAction::triggered, this,
        [this] { emit loadFileRequested(); });
    syncMenu();
    return menu;
}

QComboBox* PaletteController::createSelector(QWidget* parent)
{
    auto* selector = new QComboBox(parent);
    m_selector = selector;
    const QFontMetrics metrics(selector->font());
    int widestBuiltin = 0;
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        const auto label = builtinPaletteLabel(index);
        // Reserve room for the reversed form ("Plasma_r") so the closed
        // selector never elides the "_r" suffix syncSelector appends.
        widestBuiltin = std::max(widestBuiltin,
            metrics.horizontalAdvance(label + QStringLiteral("_r")));
        selector->addItem(label, static_cast<int>(index));
    }
    // A toggle that reverses the selected palette, kept at the end of the
    // popup. Its label reflects the state (see syncSelector); it is never the
    // closed selection, so it does not affect the fixed width below.
    selector->insertSeparator(selector->count());
    selector->addItem(tr("Reverse Colormap"), reverseToggle);
    // Size the closed combo to exactly fit the longest builtin name (the popup
    // expands independently, so the custom entry is never truncated there).
    // Any custom entry shows elided when closed.
    QStyleOptionComboBox option;
    option.initFrom(selector);
    const QSize content(widestBuiltin + 4, metrics.height());
    selector->setFixedWidth(selector->style()
            ->sizeFromContents(QStyle::CT_ComboBox, &option, content, selector)
            .width());
    selector->view()->setItemDelegate(
        new CurrentRowBulletDelegate(selector, selector->view()));
    connect(selector, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this](int) {
            if (!m_selector) {
                return;
            }
            const auto selection = m_selector->currentData().toInt();
            if (selection >= 0) {
                selectBuiltin(selection);
            } else if (selection == reverseToggle) {
                // Flip the state; syncSelector then restores the current index
                // to the palette.
                setReversed(!m_state.reversed);
            }
            // The transient custom entry is already the selection: a no-op.
        });
    syncSelector();
    return selector;
}

void PaletteController::selectBuiltin(int index)
{
    if (index < 0 || index >= static_cast<int>(builtinPalettes.size())) {
        return;
    }
    State state = m_state;
    state.builtinIndex = index;
    state.fromFile = false;
    state.filePath.clear();
    m_unloadedFilePath.clear();
    install(builtinPalette(builtinPalettes[static_cast<std::size_t>(index)]),
        std::move(state));
}

std::optional<QString> PaletteController::loadFile(const QString& path)
{
    Palette loaded;
    try {
        loaded = Palette::load(path.toStdString());
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    }
    State state = m_state;
    state.fromFile = true;
    state.filePath = path;
    m_unloadedFilePath.clear();
    install(std::move(loaded), std::move(state));
    return std::nullopt;
}

void PaletteController::setReversed(bool reversed)
{
    if (reversed == m_state.reversed) {
        return;
    }
    State state = m_state;
    state.reversed = reversed;
    install(m_basePalette, std::move(state));
}

std::optional<QString> PaletteController::apply(const State& requested)
{
    State state = requested;
    // An index the builtins do not have (an imported state from a build with
    // more of them, say) falls back to the first, as restore() does for an
    // unknown name -- one policy for one kind of bad input.
    if (state.builtinIndex < 0
        || state.builtinIndex >= static_cast<int>(builtinPalettes.size())) {
        state.builtinIndex = 0;
    }
    std::optional<QString> error;
    if (state.fromFile && !state.filePath.isEmpty()) {
        std::optional<Palette> loaded;
        try {
            loaded = Palette::load(state.filePath.toStdString());
        } catch (const std::exception& failure) {
            // Fall through to the builtin the state carries.
            error = QString::fromUtf8(failure.what());
        }
        if (loaded) {
            m_unloadedFilePath.clear();
            install(std::move(*loaded), std::move(state));
            return std::nullopt;
        }
    }
    // Keep the wanted file (see m_unloadedFilePath) only when there was one
    // and it failed; a state without a file replaces any earlier one.
    m_unloadedFilePath = error ? state.filePath : QString();
    state.fromFile = false;
    state.filePath.clear();
    install(builtinPalette(
                builtinPalettes[static_cast<std::size_t>(state.builtinIndex)]),
        std::move(state));
    return error;
}

std::optional<QString> PaletteController::restore(const QSettings& settings)
{
    State state;
    state.fromFile
        = settings.value(QStringLiteral("palette/fromFile"), false).toBool();
    state.filePath
        = settings.value(QStringLiteral("palette/filePath")).toString();
    const auto name = settings.value(QStringLiteral("palette/builtin"),
        QStringLiteral("rainbow")).toString();
    for (std::size_t index = 0; index < builtinPalettes.size(); ++index) {
        if (name == builtinPaletteKey(index)) {
            state.builtinIndex = static_cast<int>(index);
            break;
        }
    }
    state.reversed
        = settings.value(QStringLiteral("palette/reversed"), false).toBool();
    // Restore is not a change: block the signal the widgets and consumers are
    // not yet wired for, and let the host read palette() when it is ready.
    const QSignalBlocker blocker(this);
    return apply(state);
}

void PaletteController::save(QSettings& settings) const
{
    // A file that failed to load is still the wanted selection on disk.
    const bool fileWanted = m_state.fromFile || !m_unloadedFilePath.isEmpty();
    settings.setValue(QStringLiteral("palette/fromFile"), fileWanted);
    settings.setValue(QStringLiteral("palette/filePath"),
        m_state.fromFile ? m_state.filePath : m_unloadedFilePath);
    settings.setValue(QStringLiteral("palette/builtin"),
        builtinPaletteKey(static_cast<std::size_t>(m_state.builtinIndex)));
    settings.setValue(QStringLiteral("palette/reversed"), m_state.reversed);
}

QStringList PaletteController::menuLabels() const
{
    QStringList labels;
    if (!m_menuGroup) {
        return labels;
    }
    for (const auto* action : m_menuGroup->actions()) {
        labels.append(action->text());
    }
    return labels;
}

QStringList PaletteController::selectorLabels() const
{
    QStringList labels;
    if (!m_selector) {
        return labels;
    }
    for (int item = 0; item < m_selector->count(); ++item) {
        const auto entryData = m_selector->itemData(item);
        // Only the builtins have a non-negative index; the separator, the
        // custom entry and the reverse toggle are not palette names.
        if (entryData.isValid() && entryData.toInt() >= 0) {
            labels.append(m_selector->itemText(item));
        }
    }
    return labels;
}

void PaletteController::install(Palette basePalette, State state)
{
    // The base palette is compared as well as the state: reloading the same
    // file path picks up an edited file, which the state alone cannot tell.
    const bool changed = state.builtinIndex != m_state.builtinIndex
        || state.fromFile != m_state.fromFile
        || state.filePath != m_state.filePath
        || state.reversed != m_state.reversed
        || basePalette != m_basePalette;
    m_basePalette = std::move(basePalette);
    m_state = std::move(state);
    m_palette = m_state.reversed ? m_basePalette.reversed() : m_basePalette;
    // Always resync: a click on the checked action of the ExclusiveOptional
    // menu group has just unchecked it, no-op or not.
    syncMenu();
    syncSelector();
    if (changed) {
        emit paletteChanged();
    }
}

void PaletteController::syncMenu()
{
    if (m_menuGroup) {
        const auto actions = m_menuGroup->actions();
        for (int index = 0; index < actions.size(); ++index) {
            actions[index]->setChecked(
                !m_state.fromFile && index == m_state.builtinIndex);
        }
    }
    if (m_reverseAction) {
        const QSignalBlocker blocker(m_reverseAction);
        m_reverseAction->setChecked(m_state.reversed);
    }
}

void PaletteController::syncSelector()
{
    if (!m_selector) {
        return;
    }
    const QSignalBlocker blocker(m_selector);
    // Drop any stale custom-file entry before reconciling.
    const int custom = m_selector->findData(customEntry);
    if (custom >= 0) {
        m_selector->removeItem(custom);
    }
    // Reversal is a global modifier, so every palette name carries the "_r"
    // suffix (the plasma_r convention) while it is on -- including the closed
    // selector, which shows the active one.
    const QString suffix
        = m_state.reversed ? QStringLiteral("_r") : QString();
    for (int item = 0; item < m_selector->count(); ++item) {
        const auto entryData = m_selector->itemData(item);
        if (!entryData.isValid()) {
            continue;  // separator
        }
        const auto value = entryData.toInt();
        if (value >= 0) {
            m_selector->setItemText(item,
                builtinPaletteLabel(static_cast<std::size_t>(value)) + suffix);
        } else if (value == reverseToggle) {
            // The toggle item shows a check mark while reversal is on.
            m_selector->setItemText(item,
                (m_state.reversed ? QStringLiteral("✓ ") : QString())
                    + tr("Reverse Colormap"));
        }
    }
    if (m_state.fromFile) {
        const auto label
            = tr("Custom: %1").arg(QFileInfo(m_state.filePath).fileName())
            + suffix;
        // Insert just after the builtins (and before the separator) so the
        // reverse toggle stays anchored at the bottom.
        m_selector->insertItem(
            static_cast<int>(builtinPalettes.size()), label, customEntry);
        m_selector->setCurrentIndex(m_selector->findData(customEntry));
    } else {
        m_selector->setCurrentIndex(
            m_selector->findData(m_state.builtinIndex));
    }
}

} // namespace amrvis::qt
