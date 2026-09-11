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
    request.mappedGridSupersample = 4;
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
    // Pitch per axis = dx/4 = dz/4 = 0.0625: 16 columns over x in [0, 1]; the
    // z span runs from the lowest node (i = 0, k = 0) to the top layer at z = 1.
    require(mapped.image.width == 16, "the warped raster spans x at the supersampled pitch");
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

    // Every opaque display pixel names a raster pixel whose column matches
    // its x position (x is unstretched) and whose row brackets its z between
    // that cell's node rows.
    const auto& index = *mapped.displaySourceIndex;
    std::size_t opaque = 0;
    for (int row = 0; row < mapped.image.height; ++row) {
        for (int col = 0; col < mapped.image.width; ++col) {
            const auto pixel = static_cast<std::size_t>(row)
                    * static_cast<std::size_t>(mapped.image.width)
                + static_cast<std::size_t>(col);
            const auto source = index[pixel];
            if (source < 0) {
                require((mapped.image.rgba[pixel] >> 24U) == 0U,
                    "a pixel without a source is transparent");
                continue;
            }
            ++opaque;
            require((mapped.image.rgba[pixel] >> 24U) == 0xFFU,
                "a pixel with a source is opaque");
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
        require(mapped.image.rgba[pixel] == flat.image.rgba[0],
            "the warp carries the cell's rendered colour");
    }

    // --- refreshCachedSlice re-warps from the cached plane ---------------
    const auto plane = std::make_shared<const amrvis::ScalarPlane>(mapped.slice.plane);
    const auto refreshed = amrvis::refreshCachedSlice(session, sliceRequest(true),
        plane, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
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
        plane, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
        amrvis::DisplayMode::Raster, 0, 0, 10, false);
    require(unchanged.rasterUnchanged, "an undirty refresh keeps the raster");
    require(unchanged.mappedGrid, "an undirty refresh still reports the grid");
    require(unchanged.displayRegion == mapped.displayRegion,
        "an undirty refresh still frames the node bounding box");
    require(unchanged.gridNodes != nullptr, "an undirty refresh still carries the nodes");
    require(!unchanged.displaySourceIndex,
        "an undirty refresh draws nothing to index");

    // Switching the display off on the cache path returns to Cartesian.
    const auto back = amrvis::refreshCachedSlice(session, sliceRequest(false),
        plane, {}, {}, amrvis::RangeMode::File, std::nullopt, false, palette,
        amrvis::DisplayMode::Raster, 0, 0, 10, true);
    require(!back.mappedGrid && back.image.width == 4 && back.image.height == 4,
        "a refresh without mappedGrid draws the logical raster");
    require(back.displayRegion == plane->physicalRegion,
        "a refresh without mappedGrid frames the logical region");
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
    spec.mappedGridSupersample = 4;
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
        // Supersample 4 on a 4x4 raster: at least the unstretched axis is
        // four pixels per cell, so the flat raster (4x4) cannot be what is
        // shown.
        require(image.width >= 4 * plane.width || image.height >= 4 * plane.height,
            "the shared-range raster is the warped one, not the flat plane");
        const auto bounds = amrvis::mappedGridDisplayBounds(
            *display.gridNodes, display.mappedAxes);
        require(bounds && display.displayRegion == *bounds,
            "the shared-range display region is the node bounding box");
        require(display.minimum == result.displays.front().minimum
                && display.maximum == result.displays.front().maximum,
            "all three panels share one range");
    }
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
        testPlainPlotfile(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
        return 1;
    }
    std::cout << "mapped-grid pipeline checks passed\n";
    return 0;
}
