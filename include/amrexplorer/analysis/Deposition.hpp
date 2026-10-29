#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/io/ParticleReader.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

// Conservative, AMR-aware cloud-in-cell deposition of particles, projected
// onto a uniform image. Every selected particle's whole weight reaches the
// image or is counted as outside it:
//
// - kernel: 2-D CIC weights over the four nearest cell centers in the image
//   axes, at the cell size of the level the particle is stored on, in the
//   cell layer along the line of sight that contains it;
// - block boundaries: weight that crosses into a neighboring grid of the same
//   level is deposited there -- grids are a storage detail;
// - coarse-fine boundaries: weight that lands where the particle's level has
//   no grid goes to the next coarser level's cell containing that point,
//   recursing as needed; weight that lands in a cell a finer level covers is
//   shared among the covering cells in proportion to their image-plane area
//   (recursing again where those are covered), so it ends on leaf cells;
// - domain boundaries: periodic axes wrap; on the others, weight that would
//   leave the domain is folded back into the boundary cell;
// - projection: each receiving cell's weight is spread over the pixels its
//   image-plane footprint overlaps, by overlap area. Pixels follow the
//   numeric projection's conventions (ProjectionResult), so the two can be
//   compared or summed.
//
// Unlike Kangaroo's per-block deposit, nothing is dropped at block,
// coarse-fine or domain boundaries.

namespace amrvis {

struct DepositionRequest {
    int axis = 2;
    std::array<double, 4> rect{};
    // Particles are selected by line-of-sight position: lower <= p < upper.
    std::array<double, 2> axisBounds{};
    std::array<int, 2> resolution{0, 0};
    std::array<bool, 3> periodic{false, false, false};
};

class ParticleDeposit {
public:
    // Throws AnalysisError for anything but a 3-D Cartesian plotfile, or
    // invalid bounds.
    ParticleDeposit(const DatasetMetadata& metadata, DepositionRequest request);

    // Deposit one particle chunk: positions per axis, optional weights (one
    // per particle; null means 1), optional mask (nonzero selects), and the
    // level the particles are stored on. Throws AnalysisError for a
    // non-finite position or weight of a selected particle.
    void add(int level, std::span<const double> x, std::span<const double> y,
        std::span<const double> z, const double* weights,
        const std::uint8_t* mask);

    [[nodiscard]] int width() const noexcept { return m_request.resolution[0]; }
    [[nodiscard]] int height() const noexcept { return m_request.resolution[1]; }
    // Deposited weight per pixel, row-major over (v, u), j = 0 at v0.
    [[nodiscard]] const std::vector<double>& image() const noexcept
    {
        return m_image;
    }
    [[nodiscard]] double outsideWeight() const noexcept { return m_outside.value(); }
    // The direct sum of the selected particles' weights.
    [[nodiscard]] double selectedWeight() const noexcept { return m_selected.value(); }
    [[nodiscard]] std::uint64_t selectedParticles() const noexcept
    {
        return m_particles;
    }
    [[nodiscard]] double pixelArea() const noexcept;

private:
    enum class Direction : std::uint8_t { Any, NoRefine, NoCoarsen };

    struct Level {
        IntBox domain;
        Real3 cellSize;
        Real3 indexOrigin;
        Int3 ratio{{1, 1, 1}};
        std::vector<IntBox> boxes;
        int bin = 1;
        std::unordered_map<std::int64_t, std::vector<std::uint32_t>> bins;
    };

    [[nodiscard]] std::int64_t binKey(const Int3& bin) const noexcept;
    [[nodiscard]] bool inGrid(int level, const Int3& cell) const;
    [[nodiscard]] bool anyGridMeets(int level, const IntBox& box) const;
    void fold(int level, Int3& cell) const noexcept;
    bool resolve(int level, Int3 cell, double weight, double axisPosition,
        Direction direction);
    void deposit(int level, const Int3& cell, double weight);

    DepositionRequest m_request;
    std::array<int, 2> m_imageAxes{0, 1};
    std::vector<Level> m_levels;
    // Neumaier-compensated: a total over millions of particles is the
    // reference the image is checked against, and a plain running sum of
    // that many terms drifts by ~1e-11.
    struct CompensatedSum {
        double sum = 0.0;
        double compensation = 0.0;
        void add(double value) noexcept;
        [[nodiscard]] double value() const noexcept { return sum + compensation; }
    };

    std::vector<double> m_image;
    CompensatedSum m_outside;
    CompensatedSum m_selected;
    std::uint64_t m_particles = 0;
};

} // namespace amrvis
