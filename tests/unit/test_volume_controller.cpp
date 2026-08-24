// VolumeController against a fake session that renders synthetic frames:
// the action's enablement follows the session; opening the window renders a
// frame sized to the view; changes during a render coalesce into one more
// render (never two outstanding); a moving camera gets a half-size,
// single-sample draft and the settled camera a full frame; reset() closes
// the window and drops a late result; a throwing session reports through
// renderFailed and a cancelled one silently.

#include "OpacityCurveWidget.hpp"
#include "PlaneMapping.hpp"
#include "VolumeController.hpp"
#include "VolumeWindow.hpp"
#include "IsoWidget.hpp"
#include "WidgetImageExport.hpp"

#include <amrexplorer/data/DatasetSession.hpp>

#include <QApplication>
#include <QColor>
#include <QAction>
#include <QRectF>
#include <QCheckBox>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QCoreApplication>
#include <QStringList>
#include <QTimer>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
    // A local-shaped session: it renders in process, so it can be told how to
    // sample. `samplingSupported` turns that off to stand in for a peer
    // speaking a protocol that has no sampling field.
    std::atomic<bool> samplingSupported{true};
    [[nodiscard]] bool supportsVolumeSampling() const noexcept override
    {
        return supportsVolumeRendering() && samplingSupported;
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
    // Every wait below is a real application.exec() (see waitFor) and the
    // volume window is usually the only top-level one, so closing it would let
    // Qt quit the application out from under the next wait. What that does to
    // an exec() differs across the Qt versions this builds against (6.4 is the
    // floor), so take the behaviour out of play rather than depend on it.
    application.setQuitOnLastWindowClosed(false);
    using amrvis::qt::VolumeController;

    // --- the size a frame is ray-cast at -------------------------------
    // A settled frame covers the view's device pixels; a draft is half the
    // view's logical size whatever the ratio, because it is thrown away.
    // Tested against ratios no platform here provides -- the whole reason the
    // ratio is a parameter.
    {
        using amrvis::qt::volumeOutputSize;
        const QSize view(640, 480);
        require(volumeOutputSize(view, 1.0, false)
                == std::array<int, 2>{640, 480},
            "a 1x settled frame is not the view's size");
        require(volumeOutputSize(view, 2.0, false)
                == std::array<int, 2>{1280, 960},
            "a 2x settled frame was not rendered at the display's pixels");
        require(volumeOutputSize(view, 1.5, false)
                == std::array<int, 2>{960, 720},
            "a fractional ratio was not applied");
        // 1.25 * 641 = 801.25 and 1.25 * 483 = 603.75: rounded, not truncated.
        require(volumeOutputSize(QSize(641, 483), 1.25, false)
                == std::array<int, 2>{801, 604},
            "a fractional ratio was truncated instead of rounded");
        // Drafts: half the LOGICAL size, and the ratio makes no difference.
        require(volumeOutputSize(view, 1.0, true)
                == std::array<int, 2>{320, 240},
            "a 1x draft is not half the view");
        require(volumeOutputSize(view, 2.0, true)
                == std::array<int, 2>{320, 240},
            "a draft was scaled by the device pixel ratio");
        require(volumeOutputSize(view, 3.0, true)
                == std::array<int, 2>{320, 240},
            "a draft was scaled by the device pixel ratio");
        // Never zero, whatever it is handed, and never past what the ray
        // caster accepts -- a 4K view at 3x would ask for 11520.
        require(volumeOutputSize(QSize(1, 1), 1.0, true)
                == std::array<int, 2>{1, 1},
            "halving a one-pixel view produced no pixels");
        require(volumeOutputSize(QSize(0, 0), 2.0, false)
                == std::array<int, 2>{1, 1},
            "an empty view produced no pixels");
        require(volumeOutputSize(QSize(3840, 2160), 3.0, false)
                == std::array<int, 2>{amrvis::maxVolumeOutputDimension,
                    amrvis::maxVolumeOutputDimension},
            "an oversized request was not bounded to what the caster takes");
        require(volumeOutputSize(view, 0.0, false)
                == std::array<int, 2>{1, 1},
            "a zero ratio was not bounded to a legal size");
    }

    // --- raster pixels to a physical box --------------------------------
    // The mapping the region of interest reads the viewport through, and the
    // one a rubber-band zoom fetches through. Its trap is the vertical flip:
    // raster rows count down, physical coordinates count up.
    {
        using amrvis::qt::physicalRegionForRasterRect;
        const amrvis::RealBox raster{amrvis::Real3{{0.0, 0.0, 100.0}},
            amrvis::Real3{{10.0, 20.0, 130.0}}};
        const std::array<int, 2> axes{0, 1};   // an XY view: x across, y up
        const auto whole = physicalRegionForRasterRect(
            raster, 40.0, 30.0, QRectF(0.0, 0.0, 40.0, 30.0), axes);
        require(whole == raster,
            "the whole raster did not map back to the whole region");

        // The left half of the raster is the low half of x.
        const auto left = physicalRegionForRasterRect(
            raster, 40.0, 30.0, QRectF(0.0, 0.0, 20.0, 30.0), axes);
        require(left.lower[0] == 0.0 && left.upper[0] == 5.0,
            "the left half of the raster is not the low half of x");
        require(left.lower[1] == 0.0 && left.upper[1] == 20.0,
            "narrowing x moved y as well");

        // The TOP rows of the raster are the HIGH half of y. Getting this
        // backwards mirrors the region about the middle of the plane, which
        // on a symmetric domain looks entirely reasonable.
        const auto top = physicalRegionForRasterRect(
            raster, 40.0, 30.0, QRectF(0.0, 0.0, 40.0, 15.0), axes);
        require(top.lower[1] == 10.0 && top.upper[1] == 20.0,
            "the top of the raster did not map to the high half of y");
        const auto bottom = physicalRegionForRasterRect(
            raster, 40.0, 30.0, QRectF(0.0, 15.0, 40.0, 15.0), axes);
        require(bottom.lower[1] == 0.0 && bottom.upper[1] == 10.0,
            "the bottom of the raster did not map to the low half of y");

        // The axis the view does not show keeps the plane's own extent, which
        // is what stops three intersected views from collapsing every axis.
        require(top.lower[2] == 100.0 && top.upper[2] == 130.0,
            "the axis the view does not display was narrowed");

        // A raster with no pixels has no mapping to make; the region stands.
        require(physicalRegionForRasterRect(
                    raster, 0.0, 30.0, QRectF(0.0, 0.0, 1.0, 1.0), axes)
                == raster,
            "an empty raster did not leave the region alone");
    }

    // --- the region the views leave visible ------------------------------
    // Each plane view narrows only the two axes it shows, so the answer per
    // axis is the narrower of the two views that show it.
    {
        using amrvis::qt::volumeVisibleRegion;
        const amrvis::RealBox domain{amrvis::Real3{{0.0, 0.0, 0.0}},
            amrvis::Real3{{10.0, 20.0, 30.0}}};
        // Nothing zoomed: the domain, which is what an unlimited render asks
        // for anyway, so turning the limit on changes nothing until a zoom.
        require(volumeVisibleRegion(domain, {}) == domain,
            "with no view zoomed the region is not the whole domain");

        // The XY view (normal 2) zoomed in x and y; z untouched by it.
        auto xy = domain;
        xy.lower[0] = 2.0;
        xy.upper[0] = 6.0;
        xy.lower[1] = 5.0;
        xy.upper[1] = 9.0;
        auto narrowed = volumeVisibleRegion(domain,
            {std::nullopt, std::nullopt, std::optional{xy}});
        require(narrowed.lower[0] == 2.0 && narrowed.upper[0] == 6.0
                && narrowed.lower[1] == 5.0 && narrowed.upper[1] == 9.0
                && narrowed.lower[2] == 0.0 && narrowed.upper[2] == 30.0,
            "one zoomed view did not narrow exactly the axes it shows");

        // A second view narrowing a shared axis further: the YZ view (normal
        // 0) shows y and z, so y is the tighter of the two and z is its own.
        auto yz = domain;
        yz.lower[1] = 6.0;
        yz.upper[1] = 7.0;
        yz.lower[2] = 3.0;
        yz.upper[2] = 4.0;
        narrowed = volumeVisibleRegion(domain,
            {std::optional{yz}, std::nullopt, std::optional{xy}});
        require(narrowed.lower[1] == 6.0 && narrowed.upper[1] == 7.0
                && narrowed.lower[2] == 3.0 && narrowed.upper[2] == 4.0
                && narrowed.lower[0] == 2.0 && narrowed.upper[0] == 6.0,
            "two views sharing an axis did not intersect on it");

        // Views that share no part of an axis: zoom YZ into the bottom of z
        // and XZ into the top. That axis goes back to the domain rather than
        // collapsing the box, which would not be renderable.
        auto lowZ = domain;
        lowZ.lower[2] = 0.0;
        lowZ.upper[2] = 5.0;
        auto highZ = domain;
        highZ.lower[2] = 25.0;
        highZ.upper[2] = 30.0;
        const auto disjoint = volumeVisibleRegion(domain,
            {std::optional{lowZ}, std::optional{highZ}, std::nullopt});
        require(disjoint.lower[2] == 0.0 && disjoint.upper[2] == 30.0,
            "an axis the views do not share did not fall back to the domain");
        require(disjoint.valid(3),
            "disjoint view zooms produced a box that cannot be rendered");
        // The axes they *do* agree on are still narrowed -- the fallback is
        // per axis, not the whole box.
        auto lowZAlso = lowZ;
        lowZAlso.lower[0] = 1.0;
        lowZAlso.upper[0] = 2.0;
        auto highZAlso = highZ;
        highZAlso.lower[0] = 1.0;
        highZAlso.upper[0] = 2.0;
        const auto mixed = volumeVisibleRegion(domain,
            {std::optional{lowZAlso}, std::optional{highZAlso}, std::nullopt});
        require(mixed.lower[0] == 1.0 && mixed.upper[0] == 2.0
                && mixed.lower[2] == 0.0 && mixed.upper[2] == 30.0,
            "the domain fallback threw away an axis the views did agree on");
    }

    // New geometry drops the frame that belonged to the old geometry: kept, it
    // would be drawn under a wireframe for a domain it was never sampled from,
    // and scaled by a projection that no longer describes it. Checked through
    // what the widget paints, since nothing exposes the backdrop.
    {
        FakeSession geometry(3);
        amrvis::qt::IsoWidget widget;
        widget.resize(120, 90);
        widget.setGeometry(geometry.metadata());
        widget.show();
        application.processEvents();
        QImage frame(40, 30, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(255, 0, 255));   // nothing else in the view is magenta
        widget.setBackdropImage(frame, amrvis::OrthoCamera{});
        application.processEvents();
        const auto withFrame
            = amrvis::qt::renderWidgetWithoutChildren(widget, 1.0);
        const auto magenta = [](const QImage& image) {
            int found = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    found += pixel.red() > 200 && pixel.green() < 80
                            && pixel.blue() > 200
                        ? 1 : 0;
                }
            }
            return found;
        };
        require(magenta(withFrame) > 0,
            "the backdrop was not drawn, so this cannot see it being dropped");
        // The same geometry pushed again keeps it. Every frame of a plotfile
        // sequence re-pushes the geometry it already has, and dropping the
        // frame each time is what left the volume window blank for the whole
        // of a playback: the projection is in normalised domain coordinates,
        // so an unmoved domain leaves the frame exactly where it belongs.
        widget.setGeometry(geometry.metadata());
        application.processEvents();
        require(magenta(amrvis::qt::renderWidgetWithoutChildren(widget, 1.0))
                > 0,
            "re-pushing the same geometry dropped a frame that still fits it");
        // Different 3-D geometry: the domain the frame was placed in has
        // moved, so the frame goes. What places it is datasetSampleBounds --
        // the finest level's extent -- and not physicalDomain, which that
        // function ignores whenever there are levels, so moving the extent is
        // what makes this a different geometry at all.
        auto other = geometry.metadata();
        other.levels.back().cellSize = {{1.0, 2.0, 3.0}};
        widget.setGeometry(other);
        application.processEvents();
        require(magenta(amrvis::qt::renderWidgetWithoutChildren(widget, 1.0))
                == 0,
            "new geometry kept the frame sampled from the old one");
    }

    auto session = std::make_shared<FakeSession>();
    std::shared_ptr<amrvis::DatasetSession> dataset = session;
    bool shuttingDown = false;
    bool playingSequence = false;
    std::array<std::optional<amrvis::RealBox>, 3> viewRegions{};
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
            [&viewRegions, &session] {
                return amrvis::qt::volumeVisibleRegion(
                    amrvis::datasetSampleBounds(session->metadata()),
                    viewRegions);
            },
            [&playingSequence] { return playingSequence; },
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
        const auto drafted = latest;
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
        // And that full size is the view in device pixels, not logical ones.
        // True at any ratio, so it holds in the ordinary run; the
        // volume_controller_hidpi test runs this whole file at a ratio of 2,
        // where the two differ. Read here rather than at the opening render
        // because the view's size is settled by now.
        const auto ratio = volumeWindow()->viewDevicePixelRatio();
        const auto logicalView = volumeWindow()->viewSize();
        require(latest.outputSize[0]
                    == static_cast<int>(std::lround(logicalView.width() * ratio))
                && latest.outputSize[1]
                    == static_cast<int>(std::lround(logicalView.height() * ratio)),
            "the settled frame was not rendered at the view's device pixels");
        // The draft above stayed at the logical half size, ratio or not.
        require(drafted.outputSize[0] == logicalView.width() / 2,
            "the draft was scaled by the device pixel ratio");
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
        // The frame described the window that just closed, and holds a whole
        // pixel buffer: closing drops it, as reset() does.
        require(controller.lastFrame().pixels.empty(),
            "closeWindow kept the closed window's frame");
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

    // File > Close closes the window on Ctrl+W, the same shortcut the main
    // window's Close Window carries. The sequence is spelled out here rather
    // than read back off the action, so changing it in VolumeWindow fails.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the first frame was not displayed");
        auto* const window = volumeWindow();
        require(window != nullptr, "no volume window on screen");
        auto* const closeAction = window->findChild<QAction*>(
            QStringLiteral("volumeCloseAction"));
        require(closeAction != nullptr, "the volume File > Close item is gone");
        require(closeAction->shortcut() == QKeySequence(Qt::CTRL | Qt::Key_W),
            "the volume File > Close shortcut is not Ctrl+W");
        closeAction->trigger();
        // close() hides the window at once and leaves WA_DeleteOnClose to
        // delete it a turn later. Flush that here instead of waiting on the
        // event loop: when the delete lands depends on the loop level it was
        // posted from and on the Qt version, and CI's 6.4 does not run it
        // inside waitFor at all. Deleting it now also keeps a hidden,
        // pending-delete window out of the next case's volumeWindow().
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(volumeWindow() == nullptr,
            "File > Close left the volume window open");
        // The delete is what tells the controller the user closed its window,
        // through the destroyed -> forgetWindow connection that closeWindow()
        // drops; nothing else in this file takes that branch. The dropped
        // frame is what shows the handler ran -- windowOpen() would report
        // false from its QPointer nulling itself, handler or no handler.
        require(controller.lastFrame().pixels.empty(),
            "the closed window's frame was kept");
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

        // Moving the window to a display with a different scale. The logical
        // size does not change, so no resize follows -- which means the guard
        // just above, the one that dismisses an unchanged size as layout
        // churn, would swallow it and leave a frame at the old resolution up.
        // It takes its own path for that reason, and the check above is what
        // makes this one worth having.
        //
        // Only the event half can be driven from here, and only on the Qt
        // versions that have it (6.6+). QWindow::screenChanged, the other
        // trigger and the only one on Qt 6.4, cannot be emitted from outside
        // the window it belongs to.
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        auto* const scaled = volumeWindow()->findChild<amrvis::qt::IsoWidget*>();
        require(scaled != nullptr, "no view in the volume window");
        before = session->requests.load();
        QEvent scaleChange(QEvent::DevicePixelRatioChange);
        QApplication::sendEvent(scaled, &scaleChange);
        waitFor(application, [&] { return session->requests == before + 1; },
            "a change of display scale did not render a new frame");
        require(session->requestsSoFar().back().samplesPerVoxel == 2,
            "the frame after a scale change was a draft, not the full frame");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after a scale change did not finish");
#endif
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

    // The region limit: off by default, and what it puts in the request.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the opening frame was not displayed");
        const auto domain
            = amrvis::datasetSampleBounds(session->metadata());
        require(session->requestsSoFar().back().region == domain,
            "the opening render did not cover the whole domain");

        // A zoomed view while the limit is off changes nothing: no render is
        // due, and the region stays the domain.
        auto zoomed = domain;
        zoomed.lower[0] = 0.5 * (domain.lower[0] + domain.upper[0]);
        viewRegions[2] = zoomed;
        auto before = session->requests.load();
        controller.regionChanged();
        settle(application, 200);
        require(session->requests == before,
            "a view zoom rendered with the region limit switched off");

        // Turning it on renders once, at the narrowed region.
        auto* const window = volumeWindow();
        require(window != nullptr, "no volume window on screen");
        QMetaObject::invokeMethod(window, [window] {
            window->findChild<QCheckBox*>(
                       QStringLiteral("volumeRegionLimitCheck"))
                ->setChecked(true);
        });
        application.processEvents();
        waitFor(application, [&] { return session->requests == before + 1; },
            "turning the region limit on did not render");
        require(session->requestsSoFar().back().region == zoomed,
            "the limited render did not cover the region the views show");
        require(session->requestsSoFar().back().samplesPerVoxel == 2,
            "the limited render was a draft rather than a full frame");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the limited render did not finish");

        // With it on, a view that moves renders again...
        before = session->requests.load();
        zoomed.upper[1] = 0.5 * (domain.lower[1] + domain.upper[1]);
        viewRegions[2] = zoomed;
        controller.regionChanged();
        waitFor(application, [&] { return session->requests == before + 1; },
            "a moved view did not re-render the limited region");
        require(session->requestsSoFar().back().region == zoomed,
            "the re-render did not pick up the moved region");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the re-render did not finish");

        // ...and one that has not moved does not. Every scheduled slice
        // request calls this, so a region that is where the frame on screen
        // came from must cost nothing.
        before = session->requests.load();
        controller.regionChanged();
        controller.regionChanged();
        settle(application, 200);
        require(session->requests == before,
            "an unchanged region rendered again anyway");
        controller.closeWindow();
        viewRegions = {};
    }

    // The opacity curve, driven the way a user drives it. The point of this
    // test is the chain, not the arithmetic: a press on the widget has to reach
    // the opacities in the request. The curve's own rules are pinned as pure
    // functions in test_volume_pipeline; what is checked here is that editing
    // it renders, and renders the shape that was drawn.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the opening frame was not displayed");
        auto* const window = volumeWindow();
        require(window != nullptr, "no volume window on screen");
        auto* const widget
            = window->findChild<amrvis::qt::OpacityCurveWidget*>(
                QStringLiteral("volumeOpacityCurve"));
        require(widget != nullptr, "no opacity curve in the volume window");
        require(widget->width() > 16 && widget->height() > 16,
            "the curve widget has no room, so a press cannot land in its plot");
        require(widget->curve().size() == 2,
            "the curve did not open as the two-point default");

        // The opening frame used that default, which is the linear ramp the
        // sliders used to produce.
        const auto opening = session->requestsSoFar().back().transfer.opacities;
        require(opening.size() == static_cast<std::size_t>(
                    amrvis::Palette::colorSlots),
            "the transfer function is not one entry per slot");
        require(opening.front() == 0.0F
                && std::abs(opening.back() - 1.0F) <= 1.0e-6F,
            "the default curve did not render as a full ramp");

        // A press in the upper middle of the plot: adds a point there and
        // starts dragging it, which is one gesture in the widget.
        const auto before = session->requests.load();
        const QPointF middle(0.5 * widget->width(), 0.25 * widget->height());
        QMouseEvent press(QEvent::MouseButtonPress, middle,
            widget->mapToGlobal(middle), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, middle,
            widget->mapToGlobal(middle), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &release);
        require(widget->curve().size() == 3,
            "pressing the plot did not add a control point");

        // ...which reaches a render, through rampChanged and the same
        // draft-then-settle path the camera uses. Nothing new was wired for
        // this, and that is exactly what has to be checked rather than assumed.
        waitFor(application, [&] { return session->requests > before; },
            "editing the opacity curve did not render");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after the curve edit did not finish");
        // The settle turns the drag's draft into a full frame.
        waitFor(application,
            [&] {
                return session->requestsSoFar().back().samplesPerVoxel == 2;
            },
            "the curve edit never settled into a full frame");

        // And the shape that was drawn is the shape that was rendered: the new
        // point sits at three quarters opacity in the middle of the range, so
        // the middle entry is far above the linear ramp's half.
        const auto shaped = session->requestsSoFar().back().transfer.opacities;
        const auto midEntry = shaped.size() / 2;
        require(shaped[midEntry] > opening[midEntry] + 0.1F,
            "the edited curve did not change the opacities in the request");
        require(std::abs(static_cast<double>(shaped[midEntry]) - 0.75) <= 0.06,
            "the middle entry is not the opacity the point was dropped at");

        // Dragging a point that is already there -- the gesture the whole
        // control exists for, and the one a press-to-insert test does not
        // touch. The point added above sits under `middle`, so a press there
        // takes it rather than adding another.
        auto moved = session->requests.load();
        const QPointF lower(0.5 * widget->width(), 0.75 * widget->height());
        QMouseEvent grab(QEvent::MouseButtonPress, middle,
            widget->mapToGlobal(middle), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &grab);
        require(widget->curve().size() == 3,
            "pressing an existing point added another one instead of taking "
            "it");
        QMouseEvent drag(QEvent::MouseMove, lower, widget->mapToGlobal(lower),
            Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(widget, &drag);
        QMouseEvent letGo(QEvent::MouseButtonRelease, lower,
            widget->mapToGlobal(lower), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &letGo);
        require(widget->curve()[1].opacity < 0.4,
            "dragging the point down did not lower its opacity");
        waitFor(application, [&] { return session->requests > moved; },
            "dragging a point did not render");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after the drag did not finish");
        const auto dragged = session->requestsSoFar().back().transfer.opacities;
        require(std::abs(static_cast<double>(dragged[midEntry]) - 0.25) <= 0.06,
            "the drag did not reach the opacities in the request");

        // The arrow keys: exact repeatable steps, where the drag above could
        // only leave the point wherever the cursor's pixel fell. The point the
        // press took is still the selected one, so the nudge lands on it
        // without another click.
        //
        // Quiesced first. The drag armed the settle timer and the release
        // emits nothing to stop it, so its full frame arriving later would
        // stand in for the render the keys are supposed to cause -- the same
        // trap the removal check below documents, which this check was blind
        // to until here.
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the drag's renders did not finish");
        auto nudged = session->requests.load();
        settle(application, 150);
        require(session->requests == nudged,
            "the volume was still rendering of its own accord, so the render "
            "below could not be the keys' doing");
        const auto placed = widget->curve()[1];
        const auto sendKey = [widget](int key, Qt::KeyboardModifiers modifiers) {
            QKeyEvent pressed(QEvent::KeyPress, key, modifiers);
            QApplication::sendEvent(widget, &pressed);
        };
        for (int tap = 0; tap < 3; ++tap) {
            sendKey(Qt::Key_Up, Qt::NoModifier);
        }
        require(std::abs(widget->curve()[1].opacity - (placed.opacity + 0.03))
                <= 1.0e-9,
            "three presses of Up did not raise the point by three percent");
        // Shift is the same key covering ground, ten steps rather than one.
        sendKey(Qt::Key_Right, Qt::ShiftModifier);
        const auto slot = 1.0 / static_cast<double>(amrvis::Palette::colorSlots - 1);
        require(std::abs(widget->curve()[1].position
                    - (placed.position + 10.0 * slot)) <= 1.0e-9,
            "Shift+Right did not move the point ten palette slots");
        // And a nudge renders, down the same path a drag takes.
        waitFor(application, [&] { return session->requests > nudged; },
            "nudging a point with the arrow keys did not render");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after the nudge did not finish");

        // Put it back where the drag left it, so the removal below still
        // finds a point under the cursor -- and so the keys are shown to be
        // symmetric rather than merely to move something.
        sendKey(Qt::Key_Left, Qt::ShiftModifier);
        for (int tap = 0; tap < 3; ++tap) {
            sendKey(Qt::Key_Down, Qt::NoModifier);
        }
        require(std::abs(widget->curve()[1].position - placed.position) <= 1.0e-9
                && std::abs(widget->curve()[1].opacity - placed.opacity)
                    <= 1.0e-9,
            "the opposite arrows did not return the point to where it was");

        // Quiesce before the next check. The drag armed the settle timer, and
        // its full frame arriving later would stand in for the render the
        // removal is supposed to cause -- which it did, until this was here.
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the drag's renders did not finish");
        auto removed = session->requests.load();
        settle(application, 150);
        require(session->requests == removed,
            "the volume was still rendering of its own accord, so the next "
            "check could not be the removal's doing");

        // And right-clicking a point removes it, which also has to render:
        // the volume is showing a curve that no longer exists.
        QMouseEvent unwanted(QEvent::MouseButtonPress, lower,
            widget->mapToGlobal(lower), Qt::RightButton, Qt::RightButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &unwanted);
        require(widget->curve().size() == 2,
            "right-clicking a point did not remove it");
        waitFor(application, [&] { return session->requests > removed; },
            "removing a point did not render");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after the removal did not finish");
        const auto restored = session->requestsSoFar().back().transfer.opacities;
        require(std::abs(static_cast<double>(restored[midEntry]) - 0.5) <= 0.02,
            "removing the point did not put the straight ramp back");

        // The point that was selected is the point that was just removed, so
        // there is nothing to nudge: the arrows have to do nothing rather than
        // move whichever point inherited the index. Down, because it would
        // show on the end point the stale index names, where Up would clamp
        // against the top and look innocent.
        const auto untouched = widget->curve();
        sendKey(Qt::Key_Down, Qt::NoModifier);
        require(widget->curve().size() == untouched.size()
                && widget->curve().back().opacity == untouched.back().opacity,
            "an arrow key moved a point after the selected one was removed");

        // The end points, where the curve's rules bite: an end may change
        // opacity but never position, so a sideways key on one asks for a move
        // it cannot have. That must not reach the renderer -- a held key would
        // otherwise restart the settle timer on every repeat and re-render an
        // identical volume, so the full frame would never arrive.
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the renders from the earlier edits did not finish");
        auto pinned = session->requests.load();
        settle(application, 150);
        require(session->requests == pinned,
            "the volume was still rendering of its own accord, so the checks "
            "below could not be the keys' doing");

        // Where a curve point is drawn: the plot is inset on every side by a
        // control point's half-width and the border. Worked out from the point
        // rather than assumed, so a press keeps landing on it wherever an edit
        // has left it -- pressing a corner instead only works while the point
        // is still in it, and silently hits nothing once it moves.
        const auto plotPoint
            = [widget](const amrvis::OpacityPoint& point) {
                  constexpr double inset = 4.0;
                  return QPointF(
                      inset + point.position * (widget->width() - 2.0 * inset),
                      widget->height() - inset
                          - point.opacity * (widget->height() - 2.0 * inset));
              };
        const auto lowEnd = plotPoint(widget->curve().front());
        QMouseEvent takeEnd(QEvent::MouseButtonPress, lowEnd,
            widget->mapToGlobal(lowEnd), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &takeEnd);
        QMouseEvent dropEnd(QEvent::MouseButtonRelease, lowEnd,
            widget->mapToGlobal(lowEnd), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &dropEnd);
        require(widget->curve().size() == 2,
            "the press missed the end point and added one instead");
        sendKey(Qt::Key_Left, Qt::ShiftModifier);
        sendKey(Qt::Key_Right, Qt::ShiftModifier);
        require(widget->curve().front().position == 0.0,
            "an arrow key moved the end point off the end of the range");
        settle(application, 150);
        require(session->requests == pinned,
            "a nudge that moved nothing rendered an identical volume anyway");

        // The same key up the other axis does move it, so the check above is
        // the rule biting rather than the keys being dead on an end point.
        sendKey(Qt::Key_Up, Qt::NoModifier);
        require(widget->curve().front().opacity > 0.0,
            "Up did not raise the end point, which is the one move it has");
        waitFor(application, [&] { return session->requests > pinned; },
            "raising the end point did not render");

        // An arrow carrying any other modifier belongs to whatever else may
        // want it -- the convention ImageView documents -- while the keypad
        // modifier macOS stamps on the arrow keys still has to nudge.
        // Checked one key at a time and both in the same direction: two
        // opposite nudges would cancel and pass whether they were swallowed
        // or not, which is how this first went in.
        const auto held = widget->curve().front().opacity;
        sendKey(Qt::Key_Up, Qt::ControlModifier);
        require(widget->curve().front().opacity == held,
            "Ctrl+arrow was swallowed as a nudge");
        sendKey(Qt::Key_Up, Qt::AltModifier);
        require(widget->curve().front().opacity == held,
            "Alt+arrow was swallowed as a nudge");
        sendKey(Qt::Key_Up, Qt::KeypadModifier);
        require(widget->curve().front().opacity > held,
            "the keypad modifier stopped an arrow key from nudging, which "
            "would leave this dead on macOS");

        // A drag that cannot move anything is not a render either, the same
        // rule the keys follow. Dragging an end point sideways asks for the
        // one move it refuses, so after the first move has settled its opacity
        // to whatever that height means, every further move along the same
        // height changes nothing at all. Two moves rather than one because the
        // first still carries the cursor's own opacity onto the point.
        const QPointF along(plotPoint(widget->curve().front()));
        QMouseEvent takeIt(QEvent::MouseButtonPress, along,
            widget->mapToGlobal(along), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &takeIt);
        const QPointF sideways(along.x() + 20.0, along.y());
        QMouseEvent firstMove(QEvent::MouseMove, sideways,
            widget->mapToGlobal(sideways), Qt::NoButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &firstMove);
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the drag's first move did not finish rendering");
        const auto stuck = session->requests.load();
        const QPointF further(along.x() + 40.0, along.y());
        QMouseEvent secondMove(QEvent::MouseMove, further,
            widget->mapToGlobal(further), Qt::NoButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &secondMove);
        settle(application, 200);
        require(session->requests == stuck,
            "dragging an end point sideways rendered a volume that could not "
            "have changed");
        QMouseEvent dropIt(QEvent::MouseButtonRelease, further,
            widget->mapToGlobal(further), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &dropIt);

        // And not while a drag is in flight: the next mouse move would
        // overwrite the nudge with the cursor's own position, so the key would
        // cost a render and leave nothing behind.
        const auto grabbed = widget->curve().front().opacity;
        // Where the nudges above have left it, not where it started.
        const auto raised = plotPoint(widget->curve().front());
        QMouseEvent holdEnd(QEvent::MouseButtonPress, raised,
            widget->mapToGlobal(raised), Qt::LeftButton, Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &holdEnd);
        require(widget->curve().size() == 2,
            "the press missed the raised end point and added one instead");
        sendKey(Qt::Key_Up, Qt::NoModifier);
        require(widget->curve().front().opacity == grabbed,
            "an arrow key moved a point that was being dragged");
        QMouseEvent letGoEnd(QEvent::MouseButtonRelease, raised,
            widget->mapToGlobal(raised), Qt::LeftButton, Qt::NoButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &letGoEnd);

        // Smooth sampling: on by default, and a full frame rather than a
        // draft when it changes, since it is a deliberate edit and not a
        // camera in motion.
        auto* const smoothCheck = window->findChild<QCheckBox*>(
            QStringLiteral("volumeSmoothSamplingCheck"));
        require(smoothCheck != nullptr,
            "no smooth-sampling box in the volume window");
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the earlier renders did not finish");
        require(smoothCheck->isEnabled() && smoothCheck->isChecked(),
            "the volume window did not offer smooth sampling for a session "
            "that can be told how to sample");
        require(session->requestsSoFar().back().sampling
                == amrvis::SamplingPolicy::Linear,
            "a ticked smooth-sampling box did not reach the request");
        auto smoothed = session->requests.load();
        QMetaObject::invokeMethod(
            window, [smoothCheck] { smoothCheck->setChecked(false); });
        application.processEvents();
        waitFor(application, [&] { return session->requests > smoothed; },
            "clearing smooth sampling did not render");
        require(session->requestsSoFar().back().sampling
                == amrvis::SamplingPolicy::Nearest,
            "clearing the box did not reach the request");
        require(session->requestsSoFar().back().samplesPerVoxel == 2,
            "the render after the toggle was a draft rather than a full frame");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the render after clearing smooth sampling did not finish");
        QMetaObject::invokeMethod(
            window, [smoothCheck] { smoothCheck->setChecked(true); });
        application.processEvents();

        // A session that cannot be told how to sample takes the choice away,
        // and takes the tick with it: a box left ticked but greyed would claim
        // a smoothness the picture does not have, and would re-arm itself the
        // moment a session that can sample arrived.
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the renders from re-ticking did not finish");
        session->samplingSupported = false;
        controller.configureForDataset();
        application.processEvents();
        require(!smoothCheck->isEnabled(),
            "the box stayed available for a session that cannot sample");
        require(!smoothCheck->isChecked(),
            "the box stayed ticked while unavailable, claiming a smoothness "
            "the render does not have");
        require(window->sampling() == amrvis::SamplingPolicy::Nearest,
            "an unavailable box still asked for smooth sampling");
        session->samplingSupported = true;
        controller.configureForDataset();
        application.processEvents();
        require(smoothCheck->isEnabled(),
            "the box did not come back for a session that can sample");

        // The overlay toggles, and the volume window's own default: grid boxes
        // off, because box edges crossing a translucent field read as
        // structure in it. The view has to agree with the box it is labelled
        // by -- setting a box before its connect fires no toggle, so an
        // agreement left to the two defaults matching is one nobody checks.
        auto* const boxesCheck = window->findChild<QCheckBox*>(
            QStringLiteral("volumeGridBoxesCheck"));
        auto* const outlineCheck = window->findChild<QCheckBox*>(
            QStringLiteral("volumeDomainOutlineCheck"));
        require(boxesCheck != nullptr && outlineCheck != nullptr,
            "no overlay toggles in the volume window");
        auto* const view = window->findChild<amrvis::qt::IsoWidget*>();
        require(view != nullptr, "no iso view in the volume window");
        require(!boxesCheck->isChecked() && !view->levelBoxesVisible(),
            "the volume window opened drawing the grid boxes");
        require(outlineCheck->isChecked() && view->domainOutlineVisible(),
            "the volume window opened without the domain outline");
        boxesCheck->setChecked(true);
        require(view->levelBoxesVisible(),
            "ticking the grid-boxes box did not reach the view");
        boxesCheck->setChecked(false);
        require(!view->levelBoxesVisible(),
            "clearing the grid-boxes box did not reach the view");
        // The outline the same way. Its opening state cannot be checked as
        // sharply as the boxes' -- the box and the view's own default agree
        // there, so nothing can tell a state that was pushed from one that was
        // never needed -- but the toggle reaching the view can be.
        outlineCheck->setChecked(false);
        require(!view->domainOutlineVisible(),
            "clearing the domain-outline box did not reach the view");
        outlineCheck->setChecked(true);
        require(view->domainOutlineVisible(),
            "ticking the domain-outline box did not reach the view");

        // The palette's own alpha takes over from the curve, so the curve is
        // disabled: it has no say while that box is in effect, and a control
        // that looks editable and does nothing is the defect this replaced.
        //
        // The box being enabled is the "this palette carries a ramp" bit, and
        // pushPalette sets it from the palette it hands over. Every builtin
        // carries one, this one included, so the box is already enabled here
        // and the check below is of that wiring rather than of a state the
        // test set for itself.
        auto* const alphaBox = window->findChild<QCheckBox*>(
            QStringLiteral("volumePaletteAlphaCheck"));
        require(alphaBox != nullptr, "no palette-alpha box in the volume window");
        require(widget->isEnabled(),
            "the curve is not editable with the palette-alpha box clear");

        // How far the widget's pixels sit from its own background, which is
        // almost all the palette strip: everything else on it is the theme's
        // greys, at or near that background. Measured against the background
        // rather than as colourfulness because fading a colour towards a dark
        // background barely changes how saturated it is -- a saturation
        // measure separates the two states in a light theme and not in a dark
        // one, which is a test that passes where it is run and nowhere else.
        const auto fromBackground = [widget](const QImage& image) {
            const auto base = widget->palette().color(QPalette::Base);
            double total = 0.0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    const auto pixel = image.pixelColor(x, y);
                    total += std::abs(pixel.red() - base.red())
                        + std::abs(pixel.green() - base.green())
                        + std::abs(pixel.blue() - base.blue());
                }
            }
            return total / static_cast<double>(image.width() * image.height());
        };
        const auto lively = fromBackground(widget->grab().toImage());
        // Vacuity guard: with no palette there would be no strip to fade, and
        // the comparison below would hold for the wrong reason.
        require(lively > 60.0,
            "the curve drew no palette strip, so fading it could not be "
            "checked");
        require(alphaBox->isEnabled(),
            "the controller did not offer the box for a palette that carries a "
            "ramp, so ticking it below would not be in effect");
        alphaBox->setChecked(true);
        require(!widget->isEnabled(),
            "the curve stayed editable while the palette's own alpha was the "
            "opacity source");
        // And the strip goes back with it. The line, the fill and the handles
        // all grey out on their own, through QPalette::Disabled; the strip is
        // painted from the data palette's own colours, which know nothing of
        // the widget, so at full strength the boldest thing on an inert
        // control would be the one part not saying it was inert.
        const auto inert = fromBackground(widget->grab().toImage());
        require(inert < 0.6 * lively,
            "the palette strip kept its strength on a disabled curve");
        alphaBox->setChecked(false);
        require(widget->isEnabled(),
            "the curve did not become editable again when the palette's alpha "
            "was switched off");

        // And the other way the box can stop being in effect: a palette with
        // no ramp arriving while it is ticked. The box unticks itself behind a
        // signal blocker, so its own toggle cannot re-enable the curve and
        // setPaletteHasAlpha has to.
        alphaBox->setChecked(true);
        require(!widget->isEnabled(), "the curve is editable again already");
        window->setPaletteHasAlpha(false);
        require(!alphaBox->isChecked() && !alphaBox->isEnabled(),
            "the box did not stand down for a palette with no ramp");
        require(widget->isEnabled(),
            "the curve stayed disabled after the palette that justified "
            "disabling it went away");
        // And quiesced, for the same reason the removal check above is: those
        // toggles each scheduled a render, and one still in the throttle when
        // the next check takes its baseline reads as that check's doing.
        settle(application, 500);
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the renders from toggling palette alpha did not finish");

        // The two ends are not removable -- the curve has to span the range --
        // and refusing must not pass for an edit either. Aimed at the point
        // itself: the edits above raised it off the plot's bottom-left corner,
        // and a press at the corner now misses it by more than the grab radius,
        // so this check went on passing while testing nothing.
        const auto ends = session->requests.load();
        const auto atLowEnd = plotPoint(widget->curve().front());
        QMouseEvent atEnd(QEvent::MouseButtonPress, atLowEnd,
            widget->mapToGlobal(atLowEnd), Qt::RightButton, Qt::RightButton,
            Qt::NoModifier);
        QApplication::sendEvent(widget, &atEnd);
        require(widget->curve().size() == 2,
            "an end point was removed, leaving the curve short of the range");
        settle(application, 200);
        require(session->requests == ends,
            "refusing to remove an end point rendered anyway");
        controller.closeWindow();
    }

    // Sequence playback. Every frame of a sequence lands in
    // configureForDataset, and a full-quality ray cast does not finish before
    // the next frame cancels it -- so asking for one meant no frame was ever
    // displayed and the window stayed blank for the whole run, while each
    // frame still burned a grid sample and a ray cast. Playback drafts, the
    // way a moving camera does, and leaves the frame already up in place.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the opening frame was not displayed");
        auto* const view = volumeWindow() != nullptr
            ? volumeWindow()->findChild<amrvis::qt::IsoWidget*>() : nullptr;
        require(view != nullptr, "no view in the volume window");
        application.processEvents();
        // Pixels of the frame FakeSession renders, as the view draws it:
        // counting them is how "the window is not blank" gets stated. The
        // match is exact, not near: the overlays drawn over the grey
        // background -- slice planes, level outlines -- blend to within a
        // couple of dozen levels of this colour, and a tolerance wide enough
        // to allow for them counts about 1400 of them whether a frame is
        // there or not. The frame is drawn with a smooth transform, so its
        // edge pixels blend and only its interior is exact; that is still
        // most of the view.
        const auto rendered = [view] {
            const auto image = amrvis::qt::renderWidgetWithoutChildren(*view, 1.0);
            int found = 0;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    found += image.pixelColor(x, y) == QColor(0x40, 0x80, 0xC0)
                        ? 1 : 0;
                }
            }
            return found;
        };
        require(rendered() > 0,
            "the opening frame is not on screen, so this cannot see it kept");
        const auto full = session->requestsSoFar().back().outputSize;
        require(session->requestsSoFar().back().samplesPerVoxel == 2,
            "the opening frame was not a full one");

        // A frame arriving while playback runs: drafted, and what is on
        // screen stays there until the draft replaces it.
        //
        // The render is made slow on purpose. What has to hold is that the
        // view still shows something *while* the next frame renders, so the
        // check has to happen in that window -- with a fast render the draft
        // lands first and then the assertions below pass on the new frame
        // whether the old one was dropped or not.
        session->delayMs = 400;
        playingSequence = true;
        auto before = session->requests.load();
        auto framesBefore = observed.frames;
        controller.configureForDataset();
        waitFor(application, [&] { return session->requests == before + 1; },
            "a sequence frame did not schedule a render");
        const auto drafted = session->requestsSoFar().back();
        require(drafted.samplesPerVoxel == 1
                && drafted.outputSize[0] <= full[0] / 2 + 1
                && drafted.outputSize[1] <= full[1] / 2 + 1,
            "a sequence frame asked for a full ray cast, which the next frame "
            "cancels before it finishes");
        // Still mid-render, so anything on screen is the previous frame and
        // not this one. Without this the two checks below prove nothing.
        require(controller.renderInFlight() && observed.frames == framesBefore,
            "the sequence render finished before the view could be checked");
        require(rendered() > 0,
            "a sequence frame blanked the view instead of leaving the "
            "previous frame up while the next one rendered");
        require(controller.lastFrame().width == full[0],
            "a sequence frame dropped the frame it was still displaying");
        session->delayMs = 0;
        waitFor(application, [&] { return observed.frames == framesBefore + 1; },
            "the drafted sequence frame was never displayed");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the drafted sequence frame did not finish");

        // And the contrast that keeps those two honest: with playback stopped,
        // the same call blanks the view and drops the frame, which is what a
        // dataset switch has to do.
        playingSequence = false;
        before = session->requests.load();
        controller.configureForDataset();
        require(rendered() == 0,
            "an ordinary dataset switch left the outgoing frame on screen");
        require(controller.lastFrame().pixels.empty(),
            "an ordinary dataset switch kept the outgoing frame");
        waitFor(application, [&] { return session->requests == before + 1; },
            "the dataset switch did not render");
        const auto restored = session->requestsSoFar().back();
        require(restored.samplesPerVoxel == 2 && restored.outputSize == full,
            "the render after playback stopped is still a draft");
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the full render did not finish");
        controller.closeWindow();
    }

    // Frames arriving faster than the render throttle's own interval must
    // still render. Every arrival abandons the render in flight, and doing
    // that through cancel() -- which stops the throttle -- pushed the pending
    // render out by another full interval each time, so at the frame
    // intervals the Speed slider allows (601 - value ms, down to 1) the
    // throttle never elapsed and nothing was rendered at all. That is the
    // same blank window this change is about, reached the other way.
    {
        VolumeController controller(hooks());
        Observed observed;
        observe(controller, observed);
        controller.showWindow(nullptr);
        waitFor(application, [&] { return observed.frames == 1; },
            "the opening frame was not displayed");
        playingSequence = true;
        auto before = session->requests.load();
        // Both halves of what the host does per frame, in order: the switch
        // starts before the frame has loaded, then the frame arrives. Driving
        // only the second half is what let an earlier version of this test
        // pass while production still starved -- the switch is where the
        // throttle was being stopped.
        const auto arrive = [&controller, &application](int gapMs) {
            controller.frameSwitchStarted();
            controller.configureForDataset();
            settle(application, gapMs);
        };
        // Sixteen arrivals 20 ms apart: half the throttle's interval.
        for (int arrival = 0; arrival < 16; ++arrival) {
            arrive(20);
        }
        // The count is not pinned -- a 40 ms throttle across 320 ms of
        // arrivals is around eight, and the bar below is three, because a
        // throttle that fires while a render is still in flight starts
        // nothing and waits for the next arrival to re-arm it. What matters is
        // that it is not zero, which is what stopping the throttle produced.
        //
        // Renders started, not frames displayed: at arrivals this fast every
        // render is superseded by the next arrival before it can finish, so
        // whether any particular one survives to be shown is a race, and
        // asserting on it failed on macOS. Nothing can ray-cast faster than
        // the frames arrive; what this pins is that the attempts keep coming.
        require(session->requests > before + 2,
            "frames arriving faster than the render throttle rendered nothing");

        // At arrivals slower than the throttle, frames do reach the screen.
        // This is the user-visible half.
        //
        // Waited for rather than timed. An earlier version gave each arrival
        // 150 ms and asserted afterwards, which failed on macOS: how long a
        // render takes to start and come back is not something this can put a
        // number on, and a number that holds on one machine is a flake on
        // another. Waiting for the event says the same thing without the
        // assumption -- and still fails, on the timeout, if nothing comes.
        for (int arrival = 0; arrival < 3; ++arrival) {
            const auto renders = session->requests.load();
            const auto shown = observed.frames;
            controller.frameSwitchStarted();
            controller.configureForDataset();
            waitFor(application, [&] { return session->requests > renders; },
                "a sequence frame at an ordinary speed did not render");
            waitFor(application, [&] { return observed.frames > shown; },
                "a sequence frame at an ordinary speed was never displayed");
        }
        playingSequence = false;
        waitFor(application, [&] { return !controller.renderInFlight(); },
            "the last playback render did not finish");
        controller.closeWindow();
    }
    return 0;
}
