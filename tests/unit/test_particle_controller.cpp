#include "ParticleController.hpp"

#include <amrexplorer/data/DatasetSession.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A dataset session with two particle species whose samples are synthetic:
// each request yields `pointsPerSpecies` points, or throws when `failing`,
// after an optional delay so a load can be cancelled or superseded in flight.
class FakeSession final : public amrvis::DatasetSession {
public:
    // The particles dialog offers the slice-cell filter only in 3-D, where a
    // slice has a normal to filter on; both dimensions need covering.
    explicit FakeSession(int dimension = 2)
    {
        amrvis::ParticleSpeciesMetadata electrons;
        electrons.name = "electrons";
        electrons.particleCount = 10;
        amrvis::ParticleSpeciesMetadata ions;
        ions.name = "ions";
        ions.particleCount = 20;
        m_species = {electrons, ions};
        m_metadata.dimension = dimension;
    }

    std::size_t pointsPerSpecies = 3;
    std::atomic<bool> failing{false};
    std::atomic<int> delayMs{0};
    // A request whose seed is slowSeed sleeps slowDelayMs instead: two loads
    // in flight together can be given distinct, order-independent durations.
    std::atomic<std::uint64_t> slowSeed{0};
    std::atomic<int> slowDelayMs{0};
    std::atomic<int> requests{0};
    // What the last request asked for, so the test can pin the argument
    // order the controller passes (a swap of fraction and seed must fail).
    std::mutex lastMutex;
    std::string lastSpecies;
    double lastFraction = -1.0;
    std::uint64_t lastSeed = 0;

    struct Request {
        std::string species;
        double fraction;
        std::uint64_t seed;
    };
    Request last()
    {
        const std::scoped_lock lock(lastMutex);
        return {lastSpecies, lastFraction, lastSeed};
    }

    [[nodiscard]] amrvis::DatasetId id() const noexcept override
    {
        return amrvis::DatasetId{1};
    }
    [[nodiscard]] const amrvis::DatasetMetadata& metadata() const noexcept override
    {
        return m_metadata;
    }
    [[nodiscard]] const amrvis::MetadataReadMetrics& metadataReadMetrics()
        const noexcept override
    {
        return m_readMetrics;
    }
    [[nodiscard]] const std::string& fileVersion() const noexcept override
    {
        return m_version;
    }
    [[nodiscard]] const std::vector<amrvis::ParticleSpeciesMetadata>&
    particleSpecies() const noexcept override
    {
        return m_species;
    }
    [[nodiscard]] amrvis::ViewDataResult requestView(
        const amrvis::ViewDataRequest&, amrvis::StopToken) override
    {
        throw std::logic_error("not used");
    }
    [[nodiscard]] amrvis::DatasetPage requestDatasetPage(
        const amrvis::DatasetPageRequest&, amrvis::StopToken) override
    {
        throw std::logic_error("not used");
    }
    [[nodiscard]] std::optional<amrvis::ValueRange> requestRange(
        const amrvis::RangeRequest&, amrvis::StopToken) override
    {
        return std::nullopt;
    }
    [[nodiscard]] bool rangeAvailable(
        const amrvis::RangeRequest&) const noexcept override
    {
        return false;
    }
    [[nodiscard]] amrvis::ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        amrvis::StopToken cancellation) override
    {
        {
            const std::scoped_lock lock(lastMutex);
            lastSpecies = species;
            lastFraction = fraction;
            lastSeed = seed;
        }
        ++requests;
        const auto delay = slowDelayMs.load() > 0 && seed == slowSeed.load()
            ? slowDelayMs.load()
            : delayMs.load();
        for (int waited = 0; waited < delay; waited += 5) {
            if (cancellation.stop_requested()) {
                throw std::runtime_error("cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (failing) {
            throw std::runtime_error("synthetic particle failure");
        }
        amrvis::ParticleSample sample;
        for (const auto& candidate : m_species) {
            if (candidate.name == species) {
                sample.species = candidate;
            }
        }
        for (std::size_t index = 0; index < pointsPerSpecies; ++index) {
            amrvis::ParticlePoint point;
            point.id = index;
            sample.points.push_back(point);
        }
        return sample;
    }
    [[nodiscard]] amrvis::CacheMetrics cacheMetrics() const override
    {
        return {};
    }
    [[nodiscard]] bool setCacheBudget(std::uint64_t) override { return true; }
    void clearUnpinnedCache() override {}
    void close() noexcept override {}

private:
    amrvis::DatasetMetadata m_metadata;
    amrvis::MetadataReadMetrics m_readMetrics;
    std::string m_version;
    std::vector<amrvis::ParticleSpeciesMetadata> m_species;
};

// Counts every signal the host would react to.
struct Observed {
    int overlays = 0;
    int selection = 0;
    int activity = 0;   // running sum of loadActivityChanged deltas
    int activityEvents = 0;
    int finished = 0;
    int stale = 0;
    QStringList failures;
    QStringList statuses;
};

// The caller owns `observed`: the lambdas capture it by reference for the
// controller's lifetime, so it must outlive the connections.
void observe(amrvis::qt::ParticleController& controller, Observed& observed)
{
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::overlaysChanged, &controller,
        [&observed] { ++observed.overlays; });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::sampleSelectionChanged, &controller,
        [&observed] { ++observed.selection; });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::loadActivityChanged, &controller,
        [&observed](int delta) {
            observed.activity += delta;
            ++observed.activityEvents;
        });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::loadFinished, &controller,
        [&observed] { ++observed.finished; });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::staleResultDropped, &controller,
        [&observed] { ++observed.stale; });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::loadFailed, &controller,
        [&observed](const QString& message) { observed.failures << message; });
    QObject::connect(&controller,
        &amrvis::qt::ParticleController::statusMessage, &controller,
        [&observed](const QString& message, int) {
            observed.statuses << message;
        });
}

// Runs the event loop until `done` or a timeout. Leaves the loop with
// exit(), not quit(): a QGuiApplication's quit() first closes every top-level
// window, which would hide the test hosts (and the progress bars in them)
// between two waits.
template <typename Predicate>
void waitFor(QCoreApplication& application, Predicate done, const char* what)
{
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timeout, &QTimer::timeout, &application,
        [&application, &timedOut] {
            timedOut = true;
            application.exit(0);
        });
    QObject::connect(&poll, &QTimer::timeout, &application,
        [&application, &done] {
            if (done()) {
                application.exit(0);
            }
        });
    poll.start(5);
    timeout.start(5000);
    application.exec();
    require(!timedOut, what);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::ParticleController;

    auto session = std::make_shared<FakeSession>();
    std::shared_ptr<amrvis::DatasetSession> current;   // the "open" dataset
    bool shuttingDown = false;
    const auto hooks = [&] {
        return ParticleController::Hooks{
            [&current] { return current; },
            [&shuttingDown] { return shuttingDown; },
        };
    };

    // Defaults, and what a dataset does to them: configureForDataset seeds a
    // colour per species (white, yellow, ...) and enables the action; without
    // a dataset the action is off; a reset forgets the colours again.
    {
        ParticleController controller(hooks());
        auto* action = controller.createAction(&application);
        QWidget host;
        auto* progress = controller.createProgress(&host);
        // Shown, so isVisible() reports the bar itself rather than the host.
        host.show();
        require(!action->isEnabled() && !progress->isVisible(),
            "the action starts enabled or the progress visible");
        require(controller.settings().fraction == 1.0
                && controller.settings().seed == 0
                && controller.settings().pointSize == 3
                && !controller.settings().selectionInitialized
                && controller.settings().species.empty(),
            "settings do not start at their defaults");
        require(controller.colorFor("electrons") == QColor(Qt::white),
            "an unknown species is not drawn white");
        controller.configureForDataset(false);
        require(!action->isEnabled(), "no dataset yet enabled the action");
        current = session;
        controller.configureForDataset(false);
        require(action->isEnabled(), "a dataset with species left the action off");
        require(controller.settings().colors.at("electrons") == QColor(Qt::white)
                && controller.settings().colors.at("ions") == QColor(Qt::yellow),
            "default species colours were not seeded in order");
        controller.setColor("ions", QColor(Qt::red));
        controller.configureForDataset(true);
        require(controller.settings().colors.at("ions") == QColor(Qt::red),
            "preserving the selection did not keep a chosen colour");
        controller.configureForDataset(false);
        require(controller.settings().colors.at("ions") == QColor(Qt::yellow),
            "a reset did not restore the default colour");
        current.reset();
    }

    // applySelection: identity changes (species, fraction, seed, first
    // application) ask the host to reload; a point-size or slice-cell-filter
    // change alone only redraws. setColor redraws.
    // clearSelection/resetSettings/restoreSelection shape the settings as the
    // dataset transitions need.
    {
        Observed observed;
        ParticleController controller(hooks());
        observe(controller, observed);
        // The very first application reloads even when it changes nothing
        // else: the defaults, applied, are still a selection to sample.
        controller.applySelection({}, 1.0, 3, 0, false);
        require(observed.selection == 1 && observed.overlays == 0,
            "the first application of the defaults did not ask for a reload");
        controller.applySelection({}, 1.0, 3, 0, false);
        require(observed.selection == 1 && observed.overlays == 1,
            "re-applying the defaults reloaded");
        controller.clearSelection();
        observed = Observed{};
        controller.applySelection({"ions"}, 0.5, 4, 7, true);
        require(observed.selection == 1 && observed.overlays == 0,
            "the first selection did not ask for a reload");
        require(controller.settings().selectionInitialized
                && controller.settings().species
                    == std::vector<std::string>{"ions"}
                && controller.settings().fraction == 0.5
                && controller.settings().seed == 7
                && controller.settings().pointSize == 4
                && controller.settings().sliceCellsOnly,
            "applySelection did not install the selection");
        // Point size and the filter change together, to distinct values, so
        // a swap of the two trailing arguments cannot pass: 9 is not a bool
        // and false is not 9.
        controller.applySelection({"ions"}, 0.5, 9, 7, false);
        require(observed.selection == 1 && observed.overlays == 1,
            "a point-size change reloaded instead of redrawing");
        require(controller.settings().pointSize == 9
                && !controller.settings().sliceCellsOnly,
            "the cosmetic arguments did not land where they belong");
        controller.applySelection({"ions"}, 0.5, 9, 7, true);
        require(observed.selection == 1 && observed.overlays == 2,
            "a slice-cell-filter change reloaded instead of redrawing");
        require(controller.settings().sliceCellsOnly,
            "the slice-cell filter did not install");
        controller.applySelection({"ions"}, 0.25, 9, 7, true);
        require(observed.selection == 2, "a fraction change did not reload");
        controller.applySelection({"ions"}, 0.25, 9, 8, true);
        require(observed.selection == 3, "a seed change did not reload");
        controller.applySelection({"ions", "electrons"}, 0.25, 9, 8, true);
        require(observed.selection == 4, "a species change did not reload");
        controller.setColor("ions", QColor(Qt::blue));
        require(observed.overlays == 3 && observed.selection == 4,
            "setColor did not redraw, or reloaded");
        controller.clearSelection();
        require(!controller.settings().selectionInitialized
                && controller.settings().species.empty()
                && controller.settings().fraction == 0.25
                && controller.settings().colors.contains("ions"),
            "clearSelection touched more than the species");
        controller.restoreSelection({"electrons"}, 0.75, 3, true);
        require(controller.settings().selectionInitialized
                && controller.settings().species
                    == std::vector<std::string>{"electrons"}
                && controller.settings().fraction == 0.75
                && controller.settings().seed == 3
                && controller.settings().pointSize == 9
                && controller.settings().sliceCellsOnly,
            "restoreSelection did not reinstall the spec's fields only");
        controller.resetSettings();
        require(controller.settings().colors.empty()
                && controller.settings().pointSize == 3
                && !controller.settings().sliceCellsOnly
                && controller.settings().fraction == 1.0,
            "resetSettings left something behind");
    }

    // The load itself: samples arrive on the pool, the UI is up meanwhile,
    // the bookkeeping balances, and the status reports the count.
    {
        current = session;
        Observed observed;
        ParticleController controller(hooks());
        auto* action = controller.createAction(&application);
        QWidget host;
        auto* progress = controller.createProgress(&host);
        // isVisible() (what the loading-UI checks read, as the window's
        // status bar does) needs a shown ancestor.
        host.show();
        controller.configureForDataset(false);
        observe(controller, observed);
        controller.restoreSelection({"electrons", "ions"}, 0.25, 7, true);
        controller.reload();
        require(controller.loading() && controller.loadingUiActive()
                && !action->isEnabled() && progress->isVisible(),
            "the loading UI did not come up");
        require(observed.activity == 1 && observed.overlays == 1
                && observed.statuses.size() == 1,
            "the load did not announce itself");
        waitFor(application, [&] { return observed.finished == 1; },
            "the load did not finish");
        require(!controller.loading() && controller.loadingUiSettled()
                && observed.activity == 0 && observed.activityEvents == 2,
            "the loading UI did not settle or the bookkeeping is unbalanced");
        require(controller.samples().size() == 2
                && controller.samples()[0].points.size() == 3
                && observed.overlays == 2 && observed.stale == 0
                && observed.failures.isEmpty(),
            "the samples did not arrive");
        // The session was asked for the selection as installed: species by
        // name, then fraction, then seed.
        const auto asked = session->last();
        require(session->requests == 2 && asked.species == "ions"
                && asked.fraction == 0.25 && asked.seed == 7,
            "the session was not asked for the selected fraction and seed");
        require(observed.statuses.last().contains(QStringLiteral("6")),
            "the status did not report the sampled count");
        // No species selected: settles at once without a worker.
        controller.restoreSelection({}, 1.0, 0, true);
        controller.reload();
        require(!controller.loading() && controller.samples().empty()
                && observed.activityEvents == 2 && observed.overlays == 3,
            "an empty selection ran a worker");
    }

    // cancel() during a load: the loading UI comes down and the action goes
    // back to what the dataset warrants, without waiting for the cancelled
    // load's result -- which, when it lands, is dropped as stale and leaves
    // the action alone. Installing samples from outside is silent. Without a
    // dataset, cancel() leaves the action off.
    {
        current = session;
        session->delayMs = 200;
        Observed observed;
        ParticleController controller(hooks());
        auto* action = controller.createAction(&application);
        QWidget host;
        auto* progress = controller.createProgress(&host);
        host.show();
        controller.configureForDataset(false);
        observe(controller, observed);
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        require(controller.loading() && !action->isEnabled()
                && progress->isVisible(),
            "the load did not take the action down");
        controller.cancel();
        require(!controller.loading() && !progress->isVisible()
                && action->isEnabled(),
            "cancel did not restore the action for the dataset still shown");
        const auto overlays = observed.overlays;
        std::vector<amrvis::ParticleSample> installed(2);
        installed[0].points.resize(3);
        controller.setSamples(installed);
        require(controller.samples().size() == 2
                && controller.samples()[0].points.size() == 3
                && observed.overlays == overlays,
            "setSamples did not install the samples, or redrew the overlays");
        controller.clearSamples();
        require(controller.samples().empty() && observed.overlays == overlays,
            "clearSamples did not drop the samples, or redrew the overlays");
        waitFor(application, [&] { return observed.finished == 1; },
            "the cancelled load did not finish");
        require(observed.stale == 1 && observed.failures.isEmpty()
                && action->isEnabled() && !controller.loading(),
            "the cancelled load's result was not dropped, or it touched "
            "the action");
        session->delayMs = 0;
        current.reset();
        controller.cancel();
        require(!action->isEnabled(),
            "cancel enabled the action with no dataset");
    }

    // suspendAction: the host's teardown word. It holds through cancel() --
    // a sequence open cancels twice, the second time from the frame switch
    // its open() triggers -- and through a load's settle, and only the next
    // configureForDataset lifts it.
    {
        current = session;
        Observed observed;
        ParticleController controller(hooks());
        auto* action = controller.createAction(&application);
        controller.configureForDataset(false);
        observe(controller, observed);
        require(action->isEnabled(), "a dataset with species left the action off");
        controller.suspendAction();
        require(!action->isEnabled(), "suspendAction left the action on");
        controller.cancel();
        require(!action->isEnabled(), "cancel() lifted a suspension");
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        waitFor(application, [&] { return observed.finished == 1; },
            "the load did not finish");
        require(!action->isEnabled(), "a settling load lifted a suspension");
        controller.configureForDataset(true);
        require(action->isEnabled(),
            "configureForDataset did not lift the suspension");
        require(controller.settings().colors.at("ions") == QColor(Qt::yellow),
            "the suspension round trip lost the seeded colours");
        current.reset();
    }

    // A failing load reports once, through loadFailed, and settles.
    {
        current = session;
        session->failing = true;
        Observed observed;
        ParticleController controller(hooks());
        observe(controller, observed);
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        waitFor(application, [&] { return observed.finished == 1; },
            "the failing load did not finish");
        require(observed.failures.size() == 1
                && observed.failures.front().contains(
                    QStringLiteral("synthetic particle failure"))
                && observed.stale == 0 && !controller.loading()
                && observed.activity == 0,
            "the failure was not reported once and settled");
        session->failing = false;
    }

    // A load superseded by cancel() (a frame switch, teardown) or by another
    // reload is dropped as stale, never installed, and never reported.
    {
        current = session;
        session->delayMs = 200;
        Observed observed;
        ParticleController controller(hooks());
        observe(controller, observed);
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        controller.cancel();
        require(!controller.loading(), "cancel left the loading flag up");
        waitFor(application, [&] { return observed.finished == 1; },
            "the cancelled load did not finish");
        require(observed.stale == 1 && observed.failures.isEmpty()
                && controller.samples().empty() && observed.activity == 0,
            "a cancelled load was installed or reported");
        // Superseded by a second, slower reload: the first result lands
        // stale while the second is still loading and must leave the loading
        // UI up for it; only the second result installs and settles.
        auto* action = controller.createAction(&application);
        QWidget host;
        auto* progress = controller.createProgress(&host);
        host.show();
        session->delayMs = 50;
        session->slowSeed = 1;
        session->slowDelayMs = 300;
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        controller.restoreSelection({"ions"}, 1.0, 1, true);
        controller.reload();
        waitFor(application, [&] { return observed.finished == 2; },
            "the superseded load did not finish");
        require(observed.stale == 2 && controller.loading()
                && !action->isEnabled() && progress->isVisible()
                && controller.samples().empty(),
            "a stale result settled the newer load's UI");
        waitFor(application, [&] { return observed.finished == 3; },
            "the superseding load did not finish");
        require(observed.stale == 2 && controller.samples().size() == 1
                && !controller.loading() && action->isEnabled()
                && !progress->isVisible()
                && observed.activity == 0 && observed.failures.isEmpty(),
            "the superseded load was not dropped in favour of the newer one");
        session->delayMs = 0;
        session->slowDelayMs = 0;
        // Shutdown: a late result touches nothing.
        session->delayMs = 100;
        controller.reload();
        shuttingDown = true;
        waitFor(application, [&] { return observed.activity == 0; },
            "the shutdown-time load did not release its activity");
        shuttingDown = false;
        session->delayMs = 0;
    }

    // The dialog before any selection: every species starts checked.
    {
        current = session;
        ParticleController controller(hooks());
        controller.configureForDataset(false);
        QWidget host;
        controller.showDialog(&host);
        auto* dialog = host.findChild<QDialog*>(QStringLiteral("particlesDialog"));
        require(dialog != nullptr, "the dialog was not shown");
        const auto checks = dialog->findChildren<QCheckBox*>();
        require(checks.size() == 2 && checks[0]->isChecked()
                && checks[1]->isChecked(),
            "an uninitialised selection did not check every species");
        controller.closeDialog();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // The dialog: one row per species, checked per the selection, Apply
    // routes to applySelection; a second showDialog raises, not rebuilds;
    // closeDialog closes it.
    {
        current = session;
        Observed observed;
        ParticleController controller(hooks());
        controller.configureForDataset(false);
        controller.restoreSelection({"ions"}, 0.5, 3, true);
        observe(controller, observed);
        QWidget host;
        controller.showDialog(&host);
        auto* dialog = host.findChild<QDialog*>(QStringLiteral("particlesDialog"));
        require(dialog != nullptr, "the dialog was not shown");
        const auto checks = dialog->findChildren<QCheckBox*>();
        require(checks.size() == 2 && !checks[0]->isChecked()
                && checks[1]->isChecked(),
            "the species rows do not reflect the selection");
        // 2-D: the slice is the domain, so the filter would do nothing and
        // the check box is not offered -- which is also what keeps the
        // species rows above findable by type and order.
        require(dialog->findChild<QCheckBox*>(
                    QStringLiteral("particlesSliceCellsOnly")) == nullptr,
            "the slice-cell check box was offered for 2-D data");
        controller.showDialog(&host);
        require(host.findChildren<QDialog*>(QStringLiteral("particlesDialog"))
                    .size() == 1,
            "a second showDialog built a second dialog");
        checks[0]->setChecked(true);
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("particlesDialogButtons"));
        require(buttons != nullptr, "no button box");
        buttons->button(QDialogButtonBox::Apply)->click();
        require(observed.selection == 1
                && controller.settings().species
                    == std::vector<std::string>{"electrons", "ions"},
            "Apply did not route the new selection");
        // Ok applies and closes; the very next showDialog must build a new
        // dialog, not raise the closing one (WA_DeleteOnClose deletes it only
        // on the next event-loop turn, so a live-pointer check alone would
        // still see it).
        buttons->button(QDialogButtonBox::Ok)->click();
        require(observed.selection == 1 && !dialog->isVisible(),
            "Ok did not close the dialog");
        controller.showDialog(&host);
        bool reopened = false;
        for (auto* candidate :
            host.findChildren<QDialog*>(QStringLiteral("particlesDialog"))) {
            reopened = reopened || candidate->isVisible();
        }
        require(reopened, "the dialog did not reopen after Ok");
        controller.closeDialog();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(host.findChild<QDialog*>(QStringLiteral("particlesDialog"))
                    == nullptr,
            "closeDialog did not close the dialog");
    }

    // The 3-D dialog offers the slice-cell filter, shows what is stored, and
    // Apply routes it through applySelection as a redraw rather than a
    // reload: the samples it filters are already loaded.
    {
        auto volume = std::make_shared<FakeSession>(3);
        current = volume;
        Observed observed;
        ParticleController controller(hooks());
        controller.configureForDataset(false);
        controller.restoreSelection({"ions"}, 0.5, 3, true);
        observe(controller, observed);
        QWidget host;
        controller.showDialog(&host);
        auto* dialog = host.findChild<QDialog*>(QStringLiteral("particlesDialog"));
        require(dialog != nullptr, "the dialog was not shown");
        auto* filter = dialog->findChild<QCheckBox*>(
            QStringLiteral("particlesSliceCellsOnly"));
        require(filter != nullptr,
            "the slice-cell check box was not offered for 3-D data");
        require(!filter->isChecked(),
            "the check box did not start from the stored setting");
        filter->setChecked(true);
        auto* buttons = dialog->findChild<QDialogButtonBox*>(
            QStringLiteral("particlesDialogButtons"));
        require(buttons != nullptr, "no button box");
        buttons->button(QDialogButtonBox::Apply)->click();
        require(controller.settings().sliceCellsOnly,
            "Apply did not route the slice-cell filter");
        require(observed.overlays == 1 && observed.selection == 0,
            "the slice-cell filter reloaded instead of redrawing");
        // Reopened, the box shows what was applied.
        controller.closeDialog();
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        controller.showDialog(&host);
        dialog = host.findChild<QDialog*>(QStringLiteral("particlesDialog"));
        require(dialog != nullptr, "the dialog did not reopen");
        filter = dialog->findChild<QCheckBox*>(
            QStringLiteral("particlesSliceCellsOnly"));
        require(filter != nullptr && filter->isChecked(),
            "the reopened check box did not show the applied setting");
        controller.closeDialog();
        current = session;
    }

    std::cout << "particle controller tests passed\n";
    return 0;
}
