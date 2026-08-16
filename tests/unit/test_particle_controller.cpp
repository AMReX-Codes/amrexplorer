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
    FakeSession()
    {
        amrvis::ParticleSpeciesMetadata electrons;
        electrons.name = "electrons";
        electrons.particleCount = 10;
        amrvis::ParticleSpeciesMetadata ions;
        ions.name = "ions";
        ions.particleCount = 20;
        m_species = {electrons, ions};
        m_metadata.dimension = 2;
    }

    std::size_t pointsPerSpecies = 3;
    std::atomic<bool> failing{false};
    std::atomic<int> delayMs{0};
    std::atomic<int> requests{0};

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
        const std::string& species, double, std::uint64_t,
        amrvis::StopToken cancellation) override
    {
        ++requests;
        const auto delay = delayMs.load();
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

Observed observe(amrvis::qt::ParticleController& controller)
{
    Observed observed;
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
    return observed;
}

// Runs the event loop until `done` or a timeout.
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
            application.quit();
        });
    QObject::connect(&poll, &QTimer::timeout, &application,
        [&application, &done] {
            if (done()) {
                application.quit();
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
    // application) ask the host to reload; a point-size change alone only
    // redraws. setColor redraws. clearSelection/resetSettings/restoreSelection
    // shape the settings as the dataset transitions need.
    {
        ParticleController controller(hooks());
        auto observed = observe(controller);
        controller.applySelection({"ions"}, 0.5, 4, 7);
        require(observed.selection == 1 && observed.overlays == 0,
            "the first selection did not ask for a reload");
        require(controller.settings().selectionInitialized
                && controller.settings().species
                    == std::vector<std::string>{"ions"}
                && controller.settings().fraction == 0.5
                && controller.settings().seed == 7
                && controller.settings().pointSize == 4,
            "applySelection did not install the selection");
        controller.applySelection({"ions"}, 0.5, 9, 7);
        require(observed.selection == 1 && observed.overlays == 1,
            "a point-size change reloaded instead of redrawing");
        controller.applySelection({"ions"}, 0.25, 9, 7);
        require(observed.selection == 2, "a fraction change did not reload");
        controller.applySelection({"ions"}, 0.25, 9, 8);
        require(observed.selection == 3, "a seed change did not reload");
        controller.applySelection({"ions", "electrons"}, 0.25, 9, 8);
        require(observed.selection == 4, "a species change did not reload");
        controller.setColor("ions", QColor(Qt::blue));
        require(observed.overlays == 2 && observed.selection == 4,
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
                && controller.settings().pointSize == 9,
            "restoreSelection did not reinstall the spec's fields only");
        controller.resetSettings();
        require(controller.settings().colors.empty()
                && controller.settings().pointSize == 3
                && controller.settings().fraction == 1.0,
            "resetSettings left something behind");
        // applySettings is the viewer-state import path.
        ParticleController::Settings imported;
        imported.selectionInitialized = true;
        imported.species = {"ions"};
        imported.fraction = 0.1;
        imported.seed = 42;
        imported.pointSize = 6;
        imported.colors["ions"] = QColor(Qt::green);
        controller.applySettings(imported);
        require(observed.selection == 5
                && controller.settings().seed == 42
                && controller.colorFor("ions") == QColor(Qt::green),
            "applySettings did not install and announce the selection");
        imported.pointSize = 2;
        controller.applySettings(imported);
        require(observed.selection == 5 && observed.overlays == 3,
            "a cosmetic applySettings reloaded instead of redrawing");
    }

    // The load itself: samples arrive on the pool, the UI is up meanwhile,
    // the bookkeeping balances, and the status reports the count.
    {
        current = session;
        ParticleController controller(hooks());
        auto* action = controller.createAction(&application);
        QWidget host;
        auto* progress = controller.createProgress(&host);
        // isVisible() (what the loading-UI checks read, as the window's
        // status bar does) needs a shown ancestor.
        host.show();
        controller.configureForDataset(false);
        auto observed = observe(controller);
        controller.restoreSelection({"electrons", "ions"}, 1.0, 0, true);
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
        require(observed.statuses.last().contains(QStringLiteral("6")),
            "the status did not report the sampled count");
        // No species selected: settles at once without a worker.
        controller.restoreSelection({}, 1.0, 0, true);
        controller.reload();
        require(!controller.loading() && controller.samples().empty()
                && observed.activityEvents == 2 && observed.overlays == 3,
            "an empty selection ran a worker");
    }

    // A failing load reports once, through loadFailed, and settles.
    {
        current = session;
        session->failing = true;
        ParticleController controller(hooks());
        auto observed = observe(controller);
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
        ParticleController controller(hooks());
        auto observed = observe(controller);
        controller.restoreSelection({"ions"}, 1.0, 0, true);
        controller.reload();
        controller.cancel();
        require(!controller.loading(), "cancel left the loading flag up");
        waitFor(application, [&] { return observed.finished == 1; },
            "the cancelled load did not finish");
        require(observed.stale == 1 && observed.failures.isEmpty()
                && controller.samples().empty() && observed.activity == 0,
            "a cancelled load was installed or reported");
        // Superseded by a second reload: only the second result lands.
        session->delayMs = 100;
        controller.reload();
        session->delayMs = 0;
        controller.reload();
        waitFor(application, [&] { return observed.finished == 3; },
            "the superseding loads did not finish");
        require(observed.stale == 2 && controller.samples().size() == 1
                && observed.activity == 0 && observed.failures.isEmpty(),
            "the superseded load was not dropped in favour of the newer one");
        // Shutdown: a late result touches nothing.
        session->delayMs = 100;
        controller.reload();
        shuttingDown = true;
        waitFor(application, [&] { return observed.activity == 0; },
            "the shutdown-time load did not release its activity");
        shuttingDown = false;
        session->delayMs = 0;
    }

    // The dialog: one row per species, checked per the selection, Apply
    // routes to applySelection; a second showDialog raises, not rebuilds;
    // closeDialog closes it.
    {
        current = session;
        ParticleController controller(hooks());
        controller.configureForDataset(false);
        controller.restoreSelection({"ions"}, 0.5, 3, true);
        auto observed = observe(controller);
        QWidget host;
        controller.showDialog(&host);
        auto* dialog = host.findChild<QDialog*>(QStringLiteral("particlesDialog"));
        require(dialog != nullptr, "the dialog was not shown");
        const auto checks = dialog->findChildren<QCheckBox*>();
        require(checks.size() == 2 && !checks[0]->isChecked()
                && checks[1]->isChecked(),
            "the species rows do not reflect the selection");
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

    std::cout << "particle controller tests passed\n";
    return 0;
}
