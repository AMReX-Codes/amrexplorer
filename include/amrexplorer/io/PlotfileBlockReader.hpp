#pragma once

#include <amrexplorer/core/Metadata.hpp>
#include <amrexplorer/core/Request.hpp>
#include <amrexplorer/core/StopToken.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace amrvis {

enum class FabRealPrecision : std::uint8_t {
    Single,
    Double
};

// Owns one decoded FAB component in its on-disk precision.  Indexing returns
// double so geometry, interpolation, and range calculations never inherit
// float arithmetic merely because the source plotfile is single precision.
class FabValues {
public:
    FabValues() = default;
    explicit FabValues(std::vector<float> values) noexcept;
    explicit FabValues(std::vector<double> values) noexcept;

    [[nodiscard]] FabRealPrecision precision() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t elementBytes() const noexcept;
    [[nodiscard]] std::uint64_t residentBytes() const noexcept;
    [[nodiscard]] double operator[](std::size_t index) const noexcept;

private:
    using Storage = std::variant<std::vector<float>, std::vector<double>>;
    Storage m_storage{std::vector<double>{}};
};

struct FabBlock {
    IntBox box;
    FieldId field;
    int component = 0;
    FabValues values;
};

struct BlockReadMetrics {
    std::uint64_t filesRead = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t valuesRead = 0;
};

struct BlockReadResult {
    std::shared_ptr<const FabBlock> block;
    BlockReadMetrics metrics;
};

class BlockReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ReadCancelled : public std::runtime_error {
public:
    ReadCancelled() : std::runtime_error("block read cancelled") {}
};

class PlotfileBlockReader {
public:
    PlotfileBlockReader(
        std::filesystem::path plotfile, std::shared_ptr<const DatasetMetadata> metadata);

    [[nodiscard]] BlockReadResult readBlock(
        const BlockRequest& request, StopToken cancellation = {}) const;

private:
    std::filesystem::path m_plotfile;
    std::shared_ptr<const DatasetMetadata> m_metadata;
};

} // namespace amrvis
