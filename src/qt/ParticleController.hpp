#pragma once

#include <amrexplorer/core/StopToken.hpp>
#include <amrexplorer/data/DatasetSession.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QAction;
class QDialog;
class QProgressBar;
class QWidget;

namespace amrvis::qt {

// The particle overlay's state machine extracted from MainWindow: which
// species are drawn, the sampled subset (fraction and seed), the point size
// and per-species colours, the loaded samples, and the asynchronous sample
// load with its cancellation and generation. It owns the Particles... action,
// the status-bar progress indicator and the modeless Particles dialog. The
// host draws the overlays (they need the views and the plane mapping), reacts
// to sampleSelectionChanged by reloading or restarting a sequence frame, and
// forwards the load's bookkeeping into its diagnostics.
//
// Settings is the explicit, serialisable selection; a viewer-state
// export/import round-trips it through settings()/applySettings.
class ParticleController final : public QObject {
    Q_OBJECT

public:
    static constexpr int defaultPointSize = 3;

    struct Settings {
        // False until the user (or a restored frame spec) has chosen species:
        // the dialog then defaults every species to checked.
        bool selectionInitialized = false;
        std::vector<std::string> species;
        double fraction = 1.0;
        std::uint64_t seed = 0;
        int pointSize = defaultPointSize;
        std::unordered_map<std::string, QColor> colors;
    };

    struct Hooks {
        // The open dataset, or null: the species list for the dialog and the
        // controls, and the source of a sample load.
        std::function<std::shared_ptr<DatasetSession>()> dataset;
        // True once application shutdown began; late load results are
        // dropped without touching the GUI.
        std::function<bool()> isShuttingDown;
    };

    ParticleController(Hooks hooks, QObject* parent = nullptr);

    // The View menu's Particles... action (opens the dialog; enabled while
    // the dataset has species and no load is in flight) and the status-bar
    // progress indicator shown during a load. Owned by their parents.
    QAction* createAction(QObject* parent);
    QProgressBar* createProgress(QWidget* parent);

    [[nodiscard]] const Settings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const std::vector<ParticleSample>& samples() const noexcept
    {
        return m_samples;
    }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    // The colour a species is drawn with; white when it has none (a reset
    // leaves none behind until the next dataset re-seeds the defaults).
    [[nodiscard]] QColor colorFor(const std::string& species) const;

    // The dialog's and the tests' entry point: installs the selection. A
    // change to the sampled identities (species, fraction, seed) emits
    // sampleSelectionChanged for the host to act on; a cosmetic change (point
    // size) only emits overlaysChanged.
    void applySelection(std::vector<std::string> species, double fraction,
        int pointSize, std::uint64_t seed);
    void setColor(const std::string& species, const QColor& color);
    // Restores a whole selection (viewer-state import).
    void applySettings(Settings settings);
    // Reinstalls what a restored frame spec carries (species, fraction, seed,
    // initialised), leaving colours and point size alone.
    void restoreSelection(std::vector<std::string> species, double fraction,
        std::uint64_t seed, bool selectionInitialized);
    // Drops every setting back to its default: the shared reset for the two
    // paths that install a different dataset, a plain open and a sequence
    // open (a subset chosen to make one dense dataset legible must not
    // silently decimate the next one).
    void resetSettings();
    // Forgets the chosen species only, as a dataset teardown does before a
    // restore reinstalls what its spec carries.
    void clearSelection();

    // Seeds default colours for the dataset's species (a reset first unless
    // preserveSelection), lifts any suspension, and enables the action for
    // datasets with species.
    void configureForDataset(bool preserveSelection);
    // Takes the action down until the next configureForDataset: the host's
    // word that the dataset on show is going away (a teardown, a sequence
    // open) and nothing -- a cancel(), a settling load -- may bring it back
    // in between. The action's state is otherwise derived: enabled iff the
    // dataset has species, no load is running, and it is not suspended.
    void suspendAction();

    // Installs samples a frame load produced (the host owns that load), or
    // drops them at a dataset teardown. Neither emits overlaysChanged: the
    // host redraws when its own view state is ready.
    void setSamples(std::vector<ParticleSample> samples);
    void clearSamples();
    // Reloads the samples for the current selection on the pool: cancels any
    // load in flight, clears the samples, and either finishes at once (no
    // dataset or no species) or runs loadParticleSamples with the progress
    // indicator up and the action disabled until it lands.
    void reload();
    // Cancels an in-flight load without touching samples or settings: its
    // result, should it still arrive, is dropped. The loading UI comes down
    // and the action goes back to what the current dataset warrants unless
    // suspended -- so a frame step whose frame then fails is not left
    // stranded, while a sequence open (suspended by the host) stays down
    // until its first frame lands. Frame switches, dataset teardown and
    // shutdown all go through here.
    void cancel();

    void showDialog(QWidget* parent);
    void closeDialog();

    // For tests: the progress indicator is up and the action disabled while a
    // load is in flight, and the reverse once it has settled.
    [[nodiscard]] bool loadingUiActive() const;
    [[nodiscard]] bool loadingUiSettled() const;

signals:
    // Samples, colours or point size changed: the host redraws the overlays.
    void overlaysChanged();
    // The sampled identities changed: the host invalidates any prefetched
    // sequence frame and either restarts an in-flight frame load (so the
    // selection is baked into its spec) or calls reload().
    void sampleSelectionChanged();
    // Background-load bookkeeping for the host's diagnostics (+1 when the
    // load starts, -1 when its watcher fires) and its status bar.
    void loadActivityChanged(int delta);
    void statusMessage(const QString& message, int timeoutMs);
    // The load failed for the current selection; the host reports it.
    void loadFailed(const QString& message);
    // A load result arrived after being superseded and was dropped.
    void staleResultDropped();
    // A load finished (either way): the host refreshes its diagnostics.
    void loadFinished();

private:
    void setLoadingUi(bool loading);
    void setActionEnabled(bool enabled);
    // Enabled iff the current dataset has species, no load is running, and
    // the host has not suspended it.
    void refreshActionEnabled();

    Hooks m_hooks;
    Settings m_settings;
    std::vector<ParticleSample> m_samples;
    bool m_loading = false;
    bool m_suspended = false;
    StopSource m_stopSource;
    // Bumped by every reload and cancel: a load result whose generation no
    // longer matches is stale.
    std::uint64_t m_generation = 0;
    QPointer<QAction> m_action;
    QPointer<QProgressBar> m_progress;
    QPointer<QDialog> m_dialog;
};

} // namespace amrvis::qt
