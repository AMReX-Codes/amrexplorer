// The slice pipeline on the materialized mapped-grid fixture
// (tests/data/plotfile_3d_mapped): a request that asks for the mapped grid
// comes back warped with the node bounding box as its display region and a
// source-index map, the cached-refresh path re-warps, and a plotfile without
// node positions draws its logical grid without complaint. The displacement
// recipe is the fixture materializer's (keep in sync with
// fixture_materializer/main.cpp and test_mapped_grid_query.cpp):
//   nu_x = nu_y = 0
//   nu_z(i, j, k) = 0.125 * (1 - k/kmax) * (i + j) / (imax + jmax)
// with imax = jmax = kmax = 4 on the domain [0,1]^3, dx = 0.25.

#include <amrexplorer/data/LocalDatasetSession.hpp>
#include <amrexplorer/pipeline/SlicePipeline.hpp>
#include <amrexplorer/render2d/MappedGridWarp.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

constexpr double dx = 0.25;
constexpr int imax = 4;
constexpr int jmax = 4;
constexpr int kmax = 4;

double nuZ(int i, int j, int k)
{
    return 0.125 * (1.0 - static_cast<double>(k) / kmax)
        * static_cast<double>(i + j) / static_cast<double>(imax + jmax);
}

bool near(double a, double b, double tolerance = 1e-12)
{
    return std::abs(a - b) <= tolerance;
}

// Forwards to a real session and counts how often the node plane is asked
// for: the cache path must reuse the nodes it is handed.
class CountingSession final : public amrvis::DatasetSession {
public:
    explicit CountingSession(std::shared_ptr<amrvis::DatasetSession> inner)
        : m_inner(std::move(inner))
    {
    }
    int nodeRequests = 0;

    [[nodiscard]] amrvis::DatasetId id() const noexcept override { return m_inner->id(); }
    [[nodiscard]] const amrvis::DatasetMetadata& metadata() const noexcept override
    {
        return m_inner->metadata();
    }
    [[nodiscard]] const amrvis::MetadataReadMetrics& metadataReadMetrics()
        const noexcept override
    {
        return m_inner->metadataReadMetrics();
    }
    [[nodiscard]] const std::string& fileVersion() const noexcept override
    {
        return m_inner->fileVersion();
    }
    [[nodiscard]] const std::vector<amrvis::ParticleSpeciesMetadata>&
    particleSpecies() const noexcept override
    {
        return m_inner->particleSpecies();
    }
    [[nodiscard]] amrvis::ViewDataResult requestView(
        const amrvis::ViewDataRequest& request, amrvis::StopToken cancellation) override
    {
        return m_inner->requestView(request, cancellation);
    }
    [[nodiscard]] amrvis::DatasetPage requestDatasetPage(
        const amrvis::DatasetPageRequest& request, amrvis::StopToken cancellation) override
    {
        return m_inner->requestDatasetPage(request, cancellation);
    }
    [[nodiscard]] std::optional<amrvis::ValueRange> requestRange(
        const amrvis::RangeRequest& request, amrvis::StopToken cancellation) override
    {
        return m_inner->requestRange(request, cancellation);
    }
    [[nodiscard]] bool rangeAvailable(
        const amrvis::RangeRequest& request) const noexcept override
    {
        return m_inner->rangeAvailable(request);
    }
    [[nodiscard]] amrvis::ParticleSample requestParticleSample(
        const std::string& species, double fraction, std::uint64_t seed,
        amrvis::StopToken cancellation) override
    {
        return m_inner->requestParticleSample(species, fraction, seed, cancellation);
    }
    [[nodiscard]] bool supportsMappedGrid() const noexcept override
    {
        return m_inner->supportsMappedGrid();
    }
    [[nodiscard]] amrvis::MappedGridPlane requestMappedGridPlane(
        const amrvis::MappedGridPlaneRequest& request,
        amrvis::StopToken cancellation) override
    {
        ++nodeRequests;
        return m_inner->requestMappedGridPlane(request, cancellation);
    }
    [[nodiscard]] amrvis::CacheMetrics cacheMetrics() const override
    {
        return m_inner->cacheMetrics();
    }
    [[nodiscard]] bool setCacheBudget(std::uint64_t bytes) override
    {
        return m_inner->setCacheBudget(bytes);
    }
    void clearUnpinnedCache() override { m_inner->clearUnpinnedCache(); }
    void close() noexcept override { m_inner->close(); }

private:
    std::shared_ptr<amrvis::DatasetSession> m_inner;
};

// The y-normal slice through cell j = 1 over the whole domain at one raster
// sample per cell: the plane spans x (columns) and z (rows).
amrvis::SliceRequest sliceRequest(bool mappedGrid)
{
    amrvis::SliceRequest request;
    request.dataset = amrvis::DatasetId{1};
    request.field = amrvis::FieldId{0};
    request.normalDirection = 1;
    request.physicalPosition = 0.375;
    request.visibleRegion.lower = {{0.0, 0.0, 0.0}};
    request.visibleRegion.upper = {{1.0, 1.0, 1.0}};
    request.maximumLevel = 0;
    request.outputSize = {4, 4};
    request.mappedGrid = mappedGrid;
    // Drawn at 16 x 16 display pixels over the whole node box (the window is
    // left empty), four per cell along x.
    request.displayPixels = {16, 16};
    return request;
}

// The same request drawn for one window of the plane.
amrvis::SliceRequest windowedRequest(
    double x0, double x1, double z0, double z1, std::array<int, 2> pixels)
{
    auto request = sliceRequest(true);
    request.displayWindow.lower = {{x0, 0.0, z0}};
    request.displayWindow.upper = {{x1, 1.0, z1}};
    request.displayPixels = pixels;
    return request;
}

// Node z on the j = 1 slice: the average of node layers j = 1 and j = 2.
double sliceNodeZ(int i, int k)
{
    return k * dx + 0.5 * (nuZ(i, 1, k) + nuZ(i, 2, k));
}

void testMappedFixture(const std::filesystem::path& fixture)
{
    const auto session = std::make_shared<amrvis::LocalDatasetSession>(
        fixture, amrvis::DatasetId{1}, 64ULL << 20U);
    require(session->supportsMappedGrid(), "fixture session supports the mapped grid");
    const amrvis::Palette palette;

    // --- executeSlice on the logical grid --------------------------------
    const auto flat = amrvis::executeSlice(session, sliceRequest(false),
        amrvis::RangeMode::File, std::nullopt, false, palette, {});
    require(!flat.mappedGrid, "a request without mappedGrid stays Cartesian");
    require(!flat.gridNodes && !flat.displaySourceIndex,
        "a Cartesian result carries no node plane or source index");
    require(flat.image.width == 4 && flat.image.height == 4,
        "the Cartesian raster is one pixel per cell");
    require(flat.displayRegion == flat.displayPlane().physicalRegion,
        "the Cartesian display region is the plane's region");

    // --- executeSlice on the mapped grid ---------------------------------
    const auto mapped = amrvis::executeSlice(session, sliceRequest(true),
        amrvis::RangeMode::File, std::nullopt, false, palette, {});
    require(mapped.mappedGrid, "a request with mappedGrid is drawn on the grid");
    require(mapped.gridNodes != nullptr, "the node plane travels on the result");
    require(mapped.gridNodes->width == 5 && mapped.gridNodes->height == 5,
        "a 4x4 raster has a 5x5 node plane");
    require(mapped.displaySourceIndex != nullptr, "the source index travels on the result");
    const auto pixelCount = static_cast<std::size_t>(mapped.image.width)
        * static_cast<std::size_t>(mapped.image.height);
    require(mapped.displaySourceIndex->size() == pixelCount,
        "the source index is parallel to the display image");
    require(mapped.image.rgba.size() == pixelCount, "the image storage matches its size");
    require(mapped.image.width == 16 && mapped.image.height == 16,
        "the warp is drawn at the requested display pixels");
    require(mapped.mappedBounds == mapped.displayRegion,
        "an empty window draws the whole node box");
    require(!mapped.mappedDomainBounds,
        "the domain bounds come only when asked for");
    require(mapped.displayPlane().width == 4 && mapped.displayPlane().height == 4,
        "the logical plane is untouched by the warp");

    // The display region is the node bounding box on the dataset axes: x
    // (axis 0) is unstretched, z (axis 2) starts at the lowest node, and y
    // (the normal) keeps the logical bounds.
    const auto& region = mapped.displayRegion;
    require(near(region.lower[0], 0.0) && near(region.upper[0], 1.0),
        "x bounds are the unstretched node positions");
    require(near(region.lower[1], 0.0) && near(region.upper[1], 1.0),
        "the normal axis keeps the logical bounds");
    double zLo = std::numeric_limits<double>::infinity();
    double zHi = -zLo;
    for (int k = 0; k <= kmax; ++k) {
        for (int i = 0; i <= imax; ++i) {
            zLo = std::min(zLo, sliceNodeZ(i, k));
            zHi = std::max(zHi, sliceNodeZ(i, k));
        }
    }
    require(zLo > 0.0, "the recipe lifts the bottom nodes (test premise)");
    require(near(region.lower[2], zLo) && near(region.upper[2], zHi),
        "z bounds are the node bounding box");
    require(near(mapped.gridNodes->b[0], sliceNodeZ(0, 0)),
        "the node plane carries the averaged layers");

    // Every display pixel with a source names a raster pixel whose column
    // matches its x position (x is unstretched) and whose row brackets its z
    // between that cell's node rows. Coverage antialiasing: a pixel whose
    // centre no cell covers is at most partly covered, and one a cell's
    // centre owns is at least partly drawn.
    const auto& index = *mapped.displaySourceIndex;
    std::size_t opaque = 0;
    for (int row = 0; row < mapped.image.height; ++row) {
        for (int col = 0; col < mapped.image.width; ++col) {
            const auto pixel = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(mapped.image.width)
                + static_cast<std::size_t>(col);
            const auto source = index[pixel];
            if (source < 0) {
                require((mapped.image.rgba[pixel] >> 24U) < 0xFFU,
                    "a pixel without a source is not fully covered");
                continue;
            }
            ++opaque;
            require((mapped.image.rgba[pixel] >> 24U) > 0U,
                "a pixel with a source is drawn");
            const int sourceCol = source % 4;
            const int sourceRow = source / 4;
            const double x = region.lower[0]
                + (col + 0.5) / mapped.image.width * (region.upper[0] - region.lower[0]);
            require(static_cast<int>(x / dx) == sourceCol,
                "an unstretched x maps back to its own column");
            const double z = region.lower[2]
                + (row + 0.5) / mapped.image.height * (region.upper[2] - region.lower[2]);
            const double zBelow = std::min(sliceNodeZ(sourceCol, sourceRow),
                sliceNodeZ(sourceCol + 1, sourceRow));
            const double zAbove = std::max(sliceNodeZ(sourceCol, sourceRow + 1),
                sliceNodeZ(sourceCol + 1, sourceRow + 1));
            require(z >= zBelow - 1e-9 && z <= zAbove + 1e-9,
                "a display pixel's z lies within its source cell's node rows");
        }
    }
    require(opaque > pixelCount / 2, "most of the display is covered by cells");
    require(opaque < pixelCount, "the lifted bottom leaves transparent corners");
    // Colours are carried, not recomputed: the bottom-left cell's colour is
    // found at its lowest opaque pixel.
    {
        const int col = 0;
        int row = 0;
        while (row < mapped.image.height
            && index[static_cast<std::size_t>(row) * static_cast<std::size_t>(mapped.image.width)] < 0) {
            ++row;
        }
        require(row < mapped.image.height, "column 0 has an opaque pixel");
        const auto pixel = static_cast<std::size_t>(row)
            * static_cast<std::size_t>(mapped.image.width) + static_cast<std::size_t>(col);
        require(index[pixel] == 0, "the lowest pixel of column 0 shows cell (0, 0)");
        require((mapped.image.rgba[pixel] & 0x00FFFFFFU) == (flat.image.rgba[0] & 0x00FFFFFFU),
            "the warp carries the cell's rendered colour");
    }

    // --- a window of the plane at its own pixels -------------------------
    {
        const auto windowed = amrvis::executeSlice(session,
            windowedRequest(0.25, 0.75, 0.3, 0.8, {10, 20}),
            amrvis::RangeMode::File, std::nullopt, false, palette, {});
        require(windowed.mappedGrid, "a windowed request is drawn on the grid");
        require(windowed.image.width == 10 && windowed.image.height == 20,
            "a window is drawn at its requested pixels");
        require(near(windowed.displayRegion.lower[0], 0.25)
                && near(windowed.displayRegion.upper[0], 0.75)
                && near(windowed.displayRegion.lower[2], 0.3)
                && near(windowed.displayRegion.upper[2], 0.8),
            "the display region is the window");
        require(windowed.mappedBounds == mapped.displayRegion,
            "the plane's node box is carried beside the window");
        require(windowed.displaySourceIndex
                && windowed.displaySourceIndex->size() == 200U,
            "the window's source index matches its pixels");
        // Interior of the domain: every pixel is covered by some cell.
        bool allDrawn = true;
        for (const auto colour : windowed.image.rgba) {
            allDrawn = allDrawn && (colour >> 24U) == 0xFFU;
        }
        require(allDrawn, "a window inside the domain is fully covered");
        // The cell under the window's centre is the one the plane puts at
        // x = 0.5, z = 0.55 on the slice: column 2, and the row whose node
        // rows bracket 0.55 at that column.
        const auto centre = (*windowed.displaySourceIndex)[10 * 10 + 5];
        require(centre >= 0 && centre % 4 == 2, "the window centre names column 2");
        if (centre >= 0) {
            const int sourceRow = centre / 4;
            require(0.55 >= std::min(sliceNodeZ(2, sourceRow), sliceNodeZ(3, sourceRow)) - 1e-9
                    && 0.55 <= std::max(sliceNodeZ(2, sourceRow + 1), sliceNodeZ(3, sourceRow + 1)) + 1e-9,
                "the window centre's cell brackets z = 0.55");
        }

        auto asking = windowedRequest(0.25, 0.75, 0.3, 0.8, {10, 20});
        asking.wantMappedDomainBounds = true;
        const auto withBounds = amrvis::executeSlice(session, asking,
            amrvis::RangeMode::File, std::nullopt, false, palette, {});
        require(withBounds.mappedDomainBounds.has_value(),
            "the domain bounds are answered when asked for");
        if (withBounds.mappedDomainBounds) {
            const auto& domain = *withBounds.mappedDomainBounds;
            bool same = true;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                same = same && near(domain.lower[axis], mapped.mappedBounds.lower[axis], 1e-9)
                    && near(domain.upper[axis], mapped.mappedBounds.upper[axis], 1e-9);
            }
            require(same,
                "on a whole-domain slice the domain bounds are the plane's node box");
        }
    }

    // --- a window reaching past the nodes, and one off them --------------
    {
        // x in [0.5, 1.5] at 20 pixels: the half past x = 1 stays clear and
        // the other half is drawn at the window's own pitch.
        const auto past = amrvis::executeSlice(session,
            windowedRequest(0.5, 1.5, 0.3, 0.8, {20, 10}),
            amrvis::RangeMode::File, std::nullopt, false, palette, {});
        bool split = past.mappedGrid && past.image.width == 20
            && past.image.height == 10 && past.image.rgba.size() == 200U;
        for (int row = 0; split && row < 10; ++row) {
            const auto alpha = [&past, row](int col) {
                return past.image.rgba[static_cast<std::size_t>(row * 20 + col)] >> 24U;
            };
            split = alpha(9) == 0xFFU && alpha(10) == 0U;
        }
        require(split && near(past.displayRegion.upper[0], 1.5),
            "a window past the nodes keeps its extent and draws only its cells");
        // Below the lowest node: still a warp of that window, transparent and
        // naming no cell, not the logical raster.
        const auto below = amrvis::executeSlice(session,
            windowedRequest(0.0, 1.0, -0.5, -0.1, {8, 4}),
            amrvis::RangeMode::File, std::nullopt, false, palette, {});
        bool clear = below.mappedGrid && below.image.rgba.size() == 32U
            && below.displaySourceIndex && below.displaySourceIndex->size() == 32U;
        for (std::size_t pixel = 0; clear && pixel < 32U; ++pixel) {
            clear = below.image.rgba[pixel] == 0U
                && (*below.displaySourceIndex)[pixel] == -1;
        }
        require(clear && near(below.displayRegion.lower[2], -0.5)
                && near(below.displayRegion.upper[2], -0.1),
            "a window off the nodes is a transparent warp of that window");
    }

    // --- refreshCachedSlice re-warps from the cached plane ---------------
    const auto plane = std::make_shared<const amrvis::ScalarPlane>(mapped.slice.plane);
    const auto refreshed = amrvis::refreshCachedSlice(session, sliceRequest(true),
        plane, {}, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
        amrvis::DisplayMode::Raster, 0, 0, 10, true);
    require(refreshed.mappedGrid, "a dirty refresh draws on the grid");
    require(refreshed.image.width == mapped.image.width
            && refreshed.image.height == mapped.image.height,
        "a dirty refresh reproduces the warped raster size");
    require(refreshed.image.rgba == mapped.image.rgba,
        "a dirty refresh reproduces the warped raster");
    require(refreshed.displayRegion == mapped.displayRegion,
        "a dirty refresh reproduces the display region");
    require(refreshed.displaySourceIndex
            && *refreshed.displaySourceIndex == *mapped.displaySourceIndex,
        "a dirty refresh reproduces the source index");

    const auto unchanged = amrvis::refreshCachedSlice(session, sliceRequest(true),
        plane, {}, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
        amrvis::DisplayMode::Raster, 0, 0, 10, false);
    require(unchanged.rasterUnchanged, "an undirty refresh keeps the raster");
    require(unchanged.mappedGrid, "an undirty refresh still reports the grid");
    require(unchanged.displayRegion == mapped.displayRegion,
        "an undirty refresh still frames the node bounding box");
    require(unchanged.gridNodes != nullptr, "an undirty refresh still carries the nodes");
    require(!unchanged.displaySourceIndex,
        "an undirty refresh draws nothing to index");

    // Handed the view's nodes, a refresh asks the session for nothing and
    // carries the very same nodes back, dirty or not.
    {
        const auto counting = std::make_shared<CountingSession>(session);
        const auto reused = amrvis::refreshCachedSlice(counting, sliceRequest(true),
            plane, {}, {}, mapped.gridNodes, amrvis::RangeMode::File, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 10, true);
        require(counting->nodeRequests == 0,
            "a dirty refresh with cached nodes asks for no node plane");
        require(reused.gridNodes == mapped.gridNodes,
            "a refresh with cached nodes carries those nodes");
        require(reused.displaySourceIndex
                && *reused.displaySourceIndex == *mapped.displaySourceIndex,
            "a refresh with cached nodes reproduces the source index");
        const auto kept = amrvis::refreshCachedSlice(counting, sliceRequest(true),
            plane, {}, {}, mapped.gridNodes, amrvis::RangeMode::File, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 10, false);
        require(counting->nodeRequests == 0,
            "an undirty refresh with cached nodes asks for no node plane");
        require(kept.mappedGrid && kept.gridNodes == mapped.gridNodes
                && kept.displayRegion == mapped.displayRegion,
            "an undirty refresh with cached nodes keeps the frame");
        // A new window at new pixels is a re-warp of the same nodes: no
        // query, the window's region and pixels.
        const auto moved = amrvis::refreshCachedSlice(counting,
            windowedRequest(0.0, 0.5, 0.2, 0.9, {7, 9}),
            plane, {}, {}, mapped.gridNodes, amrvis::RangeMode::File, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 10, true);
        require(counting->nodeRequests == 0,
            "a re-warp for another window asks for no node plane");
        require(moved.mappedGrid && moved.image.width == 7 && moved.image.height == 9
                && near(moved.displayRegion.upper[0], 0.5)
                && near(moved.displayRegion.lower[2], 0.2),
            "a re-warp for another window draws that window at its pixels");
        static_cast<void>(amrvis::refreshCachedSlice(counting, sliceRequest(true),
            plane, {}, {}, nullptr, amrvis::RangeMode::File, std::nullopt,
            false, palette, amrvis::DisplayMode::Raster, 0, 0, 10, true));
        require(counting->nodeRequests == 1,
            "a refresh without cached nodes asks for the node plane once");
    }

    // Switching the display off on the cache path returns to Cartesian.
    const auto back = amrvis::refreshCachedSlice(session, sliceRequest(false),
        plane, {}, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
        amrvis::DisplayMode::Raster, 0, 0, 10, true);
    require(!back.mappedGrid && back.image.width == 4 && back.image.height == 4,
        "a refresh without mappedGrid draws the logical raster");
    require(back.displayRegion == plane->physicalRegion,
        "a refresh without mappedGrid frames the logical region");
}

// A budget the two field blocks (32 cells each) fit but the two node blocks
// (75 nodes each) do not: the slice stands and the warp is given up with a
// reason, instead of the whole slice failing on the cache.
void testStarvedGridPool(const std::filesystem::path& fixture)
{
    {
        amrvis::PlotfileDataset dataset(fixture, amrvis::DatasetId{1}, 64ULL << 20U);
        require(dataset.mappedGrid() != nullptr, "the fixture has a grid dataset");
        require(dataset.mappedGrid()->cacheMetrics().budgetBytes == (64ULL << 20U),
            "the grid pool takes the whole block budget");
    }
    const auto session = std::make_shared<amrvis::LocalDatasetSession>(
        fixture, amrvis::DatasetId{1}, 1000);
    const amrvis::Palette palette;
    const auto result = amrvis::executeSlice(session, sliceRequest(true),
        amrvis::RangeMode::File, std::nullopt, false, palette, {});
    require(!result.mappedGrid, "a starved grid pool draws the logical grid");
    require(result.image.width == 4 && result.image.height == 4,
        "a starved grid pool keeps the field's raster");
    require(!result.mappedGridFallback.empty(),
        "a starved grid pool says why the warp is missing");
    require(result.mappedGridFallback.find("cache") != std::string::npos,
        "the reason names the cache");
    const auto plain = amrvis::executeSlice(session, sliceRequest(false),
        amrvis::RangeMode::File, std::nullopt, false, palette, {});
    require(plain.mappedGridFallback.empty(),
        "a Cartesian request carries no fallback reason");
}

// The same fixture with the trailing Nu_nd block cut from its Header: data
// to slice, but no node positions.
// A 3-D frame load in Visible range mode re-renders all three panels with the
// shared range after they were warped; the fresh rasters must be warped too.
void testSharedRangeFrameLoad(const std::filesystem::path& fixture)
{
    amrvis::FrameSliceSpec spec;
    spec.rangeMode = amrvis::RangeMode::Visible;
    spec.mappedGrid = true;
    // Viewport bounds: the planes stay native (4 x 4) while the warps are
    // drawn at these pixels over the whole node box.
    spec.outputSizes = {{20, 12}, {20, 12}, {20, 12}};
    spec.outputSizesAreViewportBounds = true;
    const auto result = amrvis::executeFrameLoad(
        fixture, amrvis::DatasetId{1}, spec, 64ULL << 20U, {});
    require(result.displays.size() == 3, "a 3-D frame load yields three panels");
    for (const auto& display : result.displays) {
        require(display.mappedGrid, "every panel of a mapped frame load is mapped");
        require(display.gridNodes != nullptr, "every mapped panel carries its nodes");
        const auto& image = display.image;
        require(display.displaySourceIndex != nullptr
                && display.displaySourceIndex->size()
                    == static_cast<std::size_t>(image.width)
                        * static_cast<std::size_t>(image.height),
            "the shared-range raster carries a source index of its own size");
        const auto& plane = display.displayPlane();
        require(plane.width == 4 && plane.height == 4,
            "viewport bounds leave the plane native");
        require(image.width == 20 && image.height == 12,
            "the shared-range raster is the warp at the viewport pixels, not the plane");
        const auto bounds = amrvis::mappedGridDisplayBounds(
            *display.gridNodes, display.mappedAxes);
        require(bounds && display.displayRegion == *bounds,
            "the shared-range display region is the node bounding box");
        require(display.mappedDomainBounds.has_value(),
            "a mapped frame load carries the domain bounds for the canvas");
        require(display.minimum == result.displays.front().minimum
                && display.maximum == result.displays.front().maximum,
            "all three panels share one range");
    }
}

// A frame drawn for what each view shows: the window and pixels its spec
// carries per view; a view without a window gets the whole node box.
void testFrameLoadDrawsTheViewWindows(const std::filesystem::path& fixture)
{
    amrvis::FrameSliceSpec spec;
    spec.mappedGrid = true;
    amrvis::RealBox window;
    window.lower = {{0.25, 0.0, 0.3}};
    window.upper = {{0.75, 1.0, 0.8}};
    spec.displayWindows = {amrvis::RealBox{}, window, amrvis::RealBox{}};
    spec.displayPixels = {{30, 20}, {10, 20}, {24, 24}};
    const auto result = amrvis::executeFrameLoad(
        fixture, amrvis::DatasetId{1}, spec, 64ULL << 20U, {});
    require(result.displays.size() == 3, "a 3-D frame load yields three panels");
    const auto& xz = result.displays[1];
    require(xz.mappedGrid && xz.image.width == 10 && xz.image.height == 20
            && near(xz.displayRegion.lower[0], 0.25)
            && near(xz.displayRegion.upper[0], 0.75)
            && near(xz.displayRegion.lower[2], 0.3)
            && near(xz.displayRegion.upper[2], 0.8),
        "a frame is drawn for the window its view shows, at its pixels");
    const auto& yz = result.displays[0];
    std::optional<amrvis::RealBox> yzBounds;
    if (yz.gridNodes) {
        yzBounds = amrvis::mappedGridDisplayBounds(*yz.gridNodes, yz.mappedAxes);
    }
    require(yz.mappedGrid && yz.image.width == 30 && yz.image.height == 20
            && yzBounds && yz.displayRegion == *yzBounds,
        "a view without a window gets the whole node box at its pixels");
}

void testPlainPlotfile(const std::filesystem::path& mappedFixture,
    const std::filesystem::path& scratch)
{
    std::filesystem::remove_all(scratch);
    std::filesystem::copy(mappedFixture, scratch,
        std::filesystem::copy_options::recursive);
    std::vector<std::string> lines;
    {
        std::ifstream input(scratch / "Header");
        require(static_cast<bool>(input), "could not read the copied Header");
        std::string line;
        while (std::getline(input, line)) {
            lines.push_back(line);
            if (line == "Level_0/Cell") {
                break;
            }
        }
    }
    require(!lines.empty() && lines.back() == "Level_0/Cell",
        "the fixture Header lists Level_0/Cell");
    {
        std::ofstream output(scratch / "Header", std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) {
            output << line << '\n';
        }
    }

    const auto session = std::make_shared<amrvis::LocalDatasetSession>(
        scratch, amrvis::DatasetId{1}, 64ULL << 20U);
    require(!session->supportsMappedGrid(), "the cut Header carries no grid");
    const amrvis::Palette palette;
    const auto result = amrvis::executeSlice(session, sliceRequest(true),
        amrvis::RangeMode::File, std::nullopt, false, palette, {});
    require(!result.mappedGrid, "a plotfile without nodes reports no mapped grid");
    require(result.image.width == 4 && result.image.height == 4,
        "a plotfile without nodes draws its logical raster");
    require(result.displayRegion == result.displayPlane().physicalRegion,
        "a plotfile without nodes frames the logical region");
    std::filesystem::remove_all(scratch);
}

// The Header and Nu_nd_H are intact but the node data file is damaged:
// truncated to a few bytes, then missing. The field still slices; the warp
// is given up with a reason, never the whole slice.
void testDamagedNodeData(const std::filesystem::path& mappedFixture,
    const std::filesystem::path& scratch)
{
    const amrvis::Palette palette;
    const auto check = [&](const char* what) {
        const auto session = std::make_shared<amrvis::LocalDatasetSession>(
            scratch, amrvis::DatasetId{1}, 64ULL << 20U);
        require(session->supportsMappedGrid(),
            "an intact Nu_nd_H still announces the grid");
        const auto result = amrvis::executeSlice(session, sliceRequest(true),
            amrvis::RangeMode::File, std::nullopt, false, palette, {});
        std::cerr << "  " << what << ": " << result.mappedGridFallback << '\n';
        require(!result.mappedGrid, "damaged node data draws the logical grid");
        require(result.image.width == 4 && result.image.height == 4,
            "damaged node data keeps the field's raster");
        require(result.displayRegion == result.displayPlane().physicalRegion,
            "damaged node data frames the logical region");
        require(result.mappedGridFallback.find("node positions") != std::string::npos,
            "the reason names the node positions");
    };
    std::filesystem::remove_all(scratch);
    std::filesystem::copy(mappedFixture, scratch,
        std::filesystem::copy_options::recursive);
    const auto nodeData = scratch / "Level_0" / "Nu_nd_D_00000";
    require(std::filesystem::is_regular_file(nodeData), "the fixture has node data");
    {
        std::ofstream truncate(nodeData, std::ios::binary | std::ios::trunc);
        truncate << "FAB ";
    }
    check("truncated");
    std::filesystem::remove(nodeData);
    check("missing");
    std::filesystem::remove_all(scratch);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: test_mapped_grid_pipeline <materialized mapped fixture>"
                     " <scratch directory>\n";
        return 2;
    }
    try {
        testMappedFixture(argv[1]);
        testSharedRangeFrameLoad(argv[1]);
        testFrameLoadDrawsTheViewWindows(argv[1]);
        testStarvedGridPool(argv[1]);
        testPlainPlotfile(argv[1], argv[2]);
        testDamagedNodeData(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << "mapped-grid pipeline checks passed\n";
    return 0;
}
