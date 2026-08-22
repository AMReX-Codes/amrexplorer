// VolumeController against a fake session that renders synthetic frames:
// the action's enablement follows the session; opening the window renders a
// frame sized to the view; changes during a render coalesce into one more
// render (never two outstanding); a moving camera gets a half-size,
// single-sample draft and the settled camera a full frame; reset() closes
// the window and drops a late result; a throwing session reports through
// renderFailed and a cancelled one silently.

#include "VolumeController.hpp"
#include "VolumeWindow.hpp"
#include "IsoWidget.hpp"

#include <amrexplorer/data/DatasetSession.hpp>

#include <QApplication>
#include <QAction>
#include <QCoreApplication>
#include <QStringList>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
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

// A 3-D session whose renders are synthetic: every pixel lit, the request
// recorded, after an optional delay so a render can be superseded or
// cancelled in flight; `failing` throws instead.
class FakeSession final : public amrvis::DatasetSession {
public:
    FakeSession(int dimension = 3)
    {
        m_metadata.dimension = dimension;
        m_metadata.finestLevel = 1;
        m_metadata.hasPhysicalGeometry = true;
        m_metadata.physicalDomain = amrvis::RealBox{
            amrvis::Real3{{0.0, 0.0, 0.0}}, amrvis::Real3{{1.0, 2.0, 3.0}}};
        m_metadata.fields.push_back({"density", amrvis::Centering::Cell, {}});
        for (int level = 0; level < 2; ++level) {
            amrvis::LevelMetadata entry;
            entry.level = level;
            const int cells = level == 0 ? 3 : 7;
            entry.domain = amrvis::IntBox{amrvis::Int3{{0, 0, 0}},
                amrvis::Int3{{cells, cells, cells}}, amrvis::Int3{{0, 0, 0}}};
            const auto size = level == 0 ? 0.25 : 0.125;
            entry.cellSize = {{size, 2.0 * size, 3.0 * size}};
            entry.boxes.push_back(entry.domain);
            m_metadata.levels.push_back(entry);
        }
    }

    std::atomic<bool> failing{false};
    // Renders one level coarser than asked and says so in the frame, the way
    // a server under its own cache pressure does.
    std::atomic<bool> sessionFallback{false};
    std::atomic<int> delayMs{0};
    std::atomic<int> requests{0};
    std::mutex requestsMutex;
    std::vector<amrvis::VolumeRenderRequest> recorded;

    std::vector<amrvis::VolumeRenderRequest> requestsSoFar()
    {
        const std::scoped_lock lock(requestsMutex);
        return recorded;
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
        const amrvis::RangeRequest& request, amrvis::StopToken) override
    {
        // Level-dependent on purpose: level 0 spans to 10, level 1 to 20, so
        // a range resolved for the level that was asked for instead of the
        // one that rendered shows up as the wrong maximum.
        return amrvis::ValueRange{
            0.0, 10.0 * static_cast<double>(request.maximumLevel + 1)};
    }
    [[nodiscard]] bool rangeAvailable(
        const amrvis::RangeRequest&) const noexcept override
    {
        return true;
    }
    [[nodiscard]] amrvis::ParticleSample requestParticleSample(
        const std::string&, double, std::uint64_t, amrvis::StopToken) override
    {
        throw std::logic_error("not used");
    }
    [[nodiscard]] bool supportsVolumeRendering() const noexcept override
    {
        return m_metadata.dimension == 3;
    }
    [[nodiscard]] amrvis::VolumeFrame renderVolume(
        const amrvis::VolumeRenderRequest& request,
        amrvis::StopToken cancellation) override
    {
        {
            const std::scoped_lock lock(requestsMutex);
            recorded.push_back(request);
        }
        ++requests;
        for (int waited = 0; waited < delayMs.load(); waited += 5) {
            if (cancellation.stop_requested()) {
                throw amrvis::ReadCancelled();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (failing) {
            throw std::runtime_error("synthetic volume failure");
        }
        amrvis::VolumeFrame frame;
        frame.width = request.outputSize[0];
        frame.height = request.outputSize[1];
        frame.pixels.assign(static_cast<std::size_t>(frame.width)
            * static_cast<std::size_t>(frame.height), 0xFF4080C0U);
        frame.usedRange = request.range.value_or(amrvis::VolumeRange{0.0, 1.0, false});
        frame.metrics.gridDims = {2, 2, 2};
        frame.metrics.coveredVoxels = 8;
        if (sessionFallback && request.maximumLevel > 0) {
            frame.cacheFallbackFromLevel = request.maximumLevel;
            frame.cacheFallbackToLevel = 0;
        }
        return frame;
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

struct Observed {
    int activity = 0;
    int frames = 0;
    int stale = 0;
    QStringList failures;
    QStringList statuses;
};

void observe(amrvis::qt::VolumeController& controller, Observed& observed)
{
    QObject::connect(&controller,
        &amrvis::qt::VolumeController::renderActivityChanged, &controller,
        [&observed](int delta) { observed.activity += delta; });
    QObject::connect(&controller, &amrvis::qt::VolumeController::frameDisplayed,
        &controller, [&observed] { ++observed.frames; });
    QObject::connect(&controller, &amrvis::qt::VolumeController::staleResultDropped,
        &controller, [&observed] { ++observed.stale; });
    QObject::connect(&controller, &amrvis::qt::VolumeController::renderFailed,
        &controller, [&observed](const QString& message) {
            observed.failures << message;
        });
    QObject::connect(&controller, &amrvis::qt::VolumeController::statusMessage,
        &controller, [&observed](const QString& message, int) {
            observed.statuses << message;
        });
}

// Runs the event loop until `done` or a timeout; leaves it with exit(), not
// quit(), which would close the top-level volume window between waits.
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

// The one Volume window on screen (a top-level widget: the controller
// parents it for placement only), so the test can drive its camera signals.
amrvis::qt::VolumeWindow* volumeWindow()
{
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<amrvis::qt::VolumeWindow*>(widget)) {
            return window;
        }
    }
    return nullptr;
}

// A little settling time for events that must NOT happen.
void settle(QCoreApplication& application, int milliseconds)
{
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &application,
        [&application] { application.exit(0); });
    timeout.start(milliseconds);
    application.exec();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    using amrvis::qt::VolumeController;

    auto session = std::make_shared<FakeSession>();
    std::shared_ptr<amrvis::DatasetSession> dataset = session;
    bool shuttingDown = false;
    amrvis::qt::RangeController::Selection rangeSelection;
    rangeSelection.mode = amrvis::RangeMode::User;
    rangeSelection.userRange = std::pair{0.0, 4.0};
    amrvis::LevelSelection level{amrvis::CompositionPolicy::FinestAvailable, 1};
    const auto& palette = amrvis::builtinPalette(amrvis::BuiltinPalette::Rainbow);
    const auto hooks = [&] {
        return VolumeController::Hooks{
            [&dataset] { return dataset; },
            [] { return std::optional{std::pair{amrvis::FieldId{0}, QString("density")}}; },
            [&level] { return level; },
            [&rangeSelection] { return rangeSelection; },
            [&palette]() -> const amrvis::Palette& { return palette; },
            [] { return std::array<double, 3>{0.5, 1.0, 1.5}; },
            [] { return true; },
            [&shuttingDown] { return shuttingDown; },
        };
    };

    // The action follows the session: enabled for a 3-D one, disabled with
    // none or a 2-D one, and re-derived by configureForDataset.
    {
        VolumeController controller(hooks());
        auto* action = controller.createAction(&controller);
        require(action->isEnabled(), "the action is disabled for a 3-D dataset");
        dataset = nullptr;
        controller.configureForDataset();
        require(!action->isEnabled(), "the action stayed enabled with no dataset");
        dataset = std::make_shared<FakeSession>(2);
        controller.configureForDataset();
        require(!action->isEnabled(), "the action was enabled for a 2-D dataset");
        dataset = session;
        controller.configureForDataset();
        require(action->isEnabled(), "the action did not re-enable for a 3-D dataset");
    }

    // Opening the window renders one frame at the view's size, with the range
    // resolved from the controls, and the frame reaches the window.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        require(controller.windowOpen(), "the window did not open");
        waitFor(application, [&] { return observed.frames == 1; },
            "the first frame was not displayed");
        require(session->requests == 1 && observed.activity == 0
                && observed.failures.isEmpty(),
            "the opening render did not run exactly once");
        const auto requests = session->requestsSoFar();
        require(requests.size() == 1
                && requests.front().outputSize[0] >= 320
                && requests.front().outputSize[1] >= 240
                && requests.front().range.has_value()
                && requests.front().range->minimum == 0.0
                && requests.front().range->maximum == 4.0
                && requests.front().samplesPerVoxel == 2
                && requests.front().transfer.colors.size()
                    == static_cast<std::size_t>(amrvis::Palette::colorSlots)
                && requests.front().maximumLevel == 1,
            "the opening request was not built from the view and the controls");
        require(controller.lastFrame().width == requests.front().outputSize[0]
                && controller.lastFrame().height == requests.front().outputSize[1],
            "the displayed frame is not the one the session rendered");

        // Changes during a slow render coalesce: two refreshes and a ramp
        // change while one runs make exactly one more render.
        session->delayMs = 150;
        controller.refresh();
        waitFor(application, [&] { return session->requests == 2; },
            "the refresh did not start a render");
        controller.refresh();
        controller.refresh();
        waitFor(application, [&] { return observed.frames == 3; },
            "the coalesced rerun did not display");
        settle(application, 100);
        require(session->requests == 3,
            "changes during a render did not coalesce into one rerun");
        session->delayMs = 0;

        // A moving camera: a draft at half the view size with one sample per
        // voxel, then, once it settles, a full frame.
        const auto before = session->requests.load();
        require(volumeWindow() != nullptr, "no volume window on screen");
        emit volumeWindow()->cameraChanged();
        waitFor(application, [&] { return session->requests == before + 1; },
            "the camera move did not render a draft");
        auto latest = session->requestsSoFar().back();
        require(latest.samplesPerVoxel == 1
                && latest.outputSize[0] <= requests.front().outputSize[0] / 2 + 1
                && latest.outputSize[1] <= requests.front().outputSize[1] / 2 + 1,
            "the draft frame is not half-size and single-sample");
        emit volumeWindow()->interactionEnded();
        waitFor(application, [&] { return session->requests == before + 2; },
            "the settled camera did not render a full frame");
        latest = session->requestsSoFar().back();
        require(latest.samplesPerVoxel == 2
                && latest.outputSize == requests.front().outputSize,
            "the settled frame is not full-size");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the settled render did not finish");

        // reset() during a slow render: the window closes and the late
        // result is dropped as stale, never displayed.
        session->delayMs = 150;
        const auto framesBefore = observed.frames;
        const auto staleBefore = observed.stale;
        controller.refresh();
        waitFor(application, [&] { return controller.renderInFlight(); },
            "the render before reset did not start");
        controller.reset();
        require(!controller.windowOpen(), "reset did not close the window");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render did not end after reset");
        require(observed.frames == framesBefore
                && observed.stale == staleBefore + 1,
            "a late result was displayed after reset");
        require(controller.lastFrame().pixels.empty(),
            "reset did not drop the last frame");
        session->delayMs = 0;
    }

    // A failing session reports through renderFailed once, and the window
    // stays usable; a cancelled render is silent.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        session->failing = true;
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.failures.size() == 1; },
            "the failed render was not reported");
        require(observed.frames == 0 && controller.windowOpen()
                && observed.failures.front().startsWith(
                    QStringLiteral("Cannot render the volume")),
            "the failure was reported wrongly or closed the window");
        session->failing = false;
        controller.refresh();
        waitFor(application, [&] { return observed.frames == 1; },
            "the window did not recover after a failed render");
        session->delayMs = 150;
        const auto staleBeforeCancel = observed.stale;
        controller.refresh();
        waitFor(application, [&] { return controller.renderInFlight(); },
            "the render before cancel did not start");
        controller.cancel();
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the cancelled render did not end");
        require(observed.failures.size() == 1 && observed.frames == 1
                && observed.stale == staleBeforeCancel + 1
                && controller.windowOpen(),
            "a cancelled render was reported or displayed");
        // Shutdown: a result arriving after the host began closing is dropped
        // without touching the window. The render has to be in flight before
        // shuttingDown is set -- refresh() only arms the throttle, so waiting
        // on "no render running" would be satisfied before one ever started
        // and the drop path would never be taken.
        controller.refresh();
        waitFor(application, [&] { return controller.renderInFlight(); },
            "the render before shutdown did not start");
        shuttingDown = true;
        waitFor(application, [&] { return observed.activity == 0
                                       && !controller.renderInFlight(); },
            "the render during shutdown did not end");
        require(observed.frames == 1, "a frame was displayed during shutdown");
        session->delayMs = 0;
        shuttingDown = false;
        controller.closeWindow();
        require(!controller.windowOpen(), "closeWindow left the window open");
    }

    // A camera that never stops moving still gets drafts. The render timer
    // is a throttle, not a debounce: events arriving faster than its interval
    // must not keep rearming it, or a drag renders nothing at all until the
    // mouse is released and the stale backdrop sits under a moving wireframe.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the first frame was not displayed");
        const auto before = session->requests.load();
        require(volumeWindow() != nullptr, "no volume window on screen");
        // Faster than the throttle, and never ended: exactly a drag in
        // progress. interactionEnded is deliberately not emitted.
        QTimer moving;
        QObject::connect(&moving, &QTimer::timeout, &application, [] {
            if (auto* window = volumeWindow()) {
                emit window->cameraChanged();
            }
        });
        moving.start(10);
        waitFor(application, [&] { return session->requests >= before + 2; },
            "a continuously moving camera rendered no drafts");
        moving.stop();
        require(session->requestsSoFar().back().samplesPerVoxel == 1,
            "the frames rendered while the camera moved were not drafts");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the drafts did not finish");
        controller.closeWindow();
    }

    // cancel() disarms the settle timer. Left armed it fires after the
    // cancellation and schedules a render of whatever dataset is published by
    // then -- the outgoing sequence frame's, under the incoming one's
    // geometry.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the first frame was not displayed");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the opening render did not finish");
        require(volumeWindow() != nullptr, "no volume window on screen");
        // Arms the settle timer and lets the draft finish, so what cancel()
        // meets is a settle timer left armed by a completed draft. The fake
        // counts renders for the whole run, so this waits on a delta: an
        // absolute floor is already true here and would return before the
        // throttle had fired, cancelling a draft that never ran.
        const auto beforeDraft = session->requests.load();
        emit volumeWindow()->cameraChanged();
        waitFor(application,
            [&] { return session->requests == beforeDraft + 1; },
            "the camera move did not render a draft");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the draft did not finish");
        const auto after = session->requests.load();
        controller.cancel();
        // Longer than the settle interval: nothing may start on its own.
        settle(application, 400);
        require(session->requests == after,
            "the settle timer started a render after cancel()");
        controller.closeWindow();
    }

    // A Level range with a fallback: the session renders coarser than it was
    // asked to, so the range has to be re-read for the level that actually
    // rendered. Colouring level 0's pixels with level 1's statistics is the
    // defect this guards -- the frame would claim a maximum of 20 when
    // nothing it drew came from a level that reaches past 10.
    {
        session->sessionFallback = true;
        rangeSelection.mode = amrvis::RangeMode::Level;
        rangeSelection.userRange.reset();
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        // The fake records every render of the whole run, so the count this
        // block cares about is the delta, not the size.
        const auto rendersBefore = session->requests.load();
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the frame after the fallback was not displayed");
        require(session->requests == rendersBefore + 2,
            "the one displayed frame did not cost exactly two renders: the "
            "level asked for, then the level the fallback landed on");
        const auto rendered = session->requestsSoFar();
        const auto& last = rendered.back();
        require(last.maximumLevel == 0 && last.range.has_value()
                && last.range->maximum < 15.0,
            "the repeated render did not re-resolve the range at level 0");
        require(controller.lastFrame().usedRange.maximum < 15.0,
            "the displayed frame kept the finer level's range");
        rangeSelection.mode = amrvis::RangeMode::User;
        rangeSelection.userRange = std::pair{0.0, 4.0};
        session->sessionFallback = false;
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the fallback render did not finish");
        controller.closeWindow();
    }

    // A discrete change renders in full, and a resize that does not change
    // the size renders not at all.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the first frame was not displayed");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the opening render did not finish");
        require(volumeWindow() != nullptr, "no volume window on screen");
        const auto full = session->requestsSoFar().back().outputSize;

        // The alpha checkbox is one completed choice, not a drag: it must not
        // come back as a half-size single-sample draft.
        auto before = session->requests.load();
        emit volumeWindow()->paletteAlphaChanged();
        waitFor(application, [&] { return session->requests == before + 1; },
            "the alpha toggle did not render");
        const auto toggled = session->requestsSoFar().back();
        require(toggled.samplesPerVoxel == 2 && toggled.outputSize == full,
            "a discrete alpha toggle was rendered as an interaction draft");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the alpha toggle render did not finish");

        // A resize signal that leaves the view the same size is the dock
        // relaying itself out -- showFrame writes a label in it -- and must
        // not schedule anything, or displaying a frame feeds the next render.
        before = session->requests.load();
        emit volumeWindow()->viewResized();
        settle(application, 400);
        require(session->requests == before,
            "a resize to the same size scheduled a render");
        controller.closeWindow();
    }

    // The logarithmic flag rides on the VolumeRangeChoice, not on the request
    // the controller hands over: the pipeline sets it per attempt, so nothing
    // here writes it and only the arriving request proves it got through.
    {
        rangeSelection.logarithmic = true;
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        const auto before = session->requests.load();
        controller.showWindow(nullptr);
        waitFor(application, [&] { return session->requests == before + 1; },
            "the logarithmic render did not run");
        require(session->requestsSoFar().back().logarithmic,
            "the logarithmic flag did not reach the request");
        rangeSelection.logarithmic = false;
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the logarithmic render did not finish");
        controller.closeWindow();
    }
    return 0;
}
