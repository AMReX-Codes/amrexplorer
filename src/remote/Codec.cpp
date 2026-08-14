#include "Codec.hpp"

#include <flatbuffers/verifier.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace amrvis::remote::codec {
namespace {

#define AMREXPLORER_ASSERT_PAYLOAD_VALUE(nativeName, wireName)               \
    static_assert(static_cast<std::uint8_t>(PayloadKind::nativeName)         \
        == static_cast<std::uint8_t>(fb::Payload::wireName))

AMREXPLORER_ASSERT_PAYLOAD_VALUE(None, NONE);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(HelloRequest, HelloRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(HelloResponse, HelloResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(OpenDatasetRequest, OpenDatasetRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(DatasetOpened, DatasetOpened);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(CloseDatasetRequest, CloseDatasetRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(DatasetClosed, DatasetClosed);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(SliceViewRequest, SliceViewRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(SliceViewResponse, SliceViewResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(LineViewRequest, LineViewRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(LineViewResponse, LineViewResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(DatasetPageRequest, DatasetPageRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(DatasetPageResponse, DatasetPageResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(ParticleSampleRequest, ParticleSampleRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(ParticleSampleResponse, ParticleSampleResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(RangeRequest, RangeRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(RangeResponse, RangeResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(ClearCacheRequest, ClearCacheRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(SetCacheBudgetRequest, SetCacheBudgetRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(CacheResponse, CacheResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(CancelRequest, CancelRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(CancelAcknowledged, CancelAcknowledged);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(PingRequest, PingRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(PongResponse, PongResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(ErrorResponse, ErrorResponse);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(ListDirectoryRequest, ListDirectoryRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(DirectoryListing, DirectoryListing);

#undef AMREXPLORER_ASSERT_PAYLOAD_VALUE

#define AMREXPLORER_ASSERT_ERROR_VALUE(name)                                \
    static_assert(static_cast<std::uint16_t>(ErrorCode::name)               \
        == static_cast<std::uint16_t>(fb::ErrorCode::name))

AMREXPLORER_ASSERT_ERROR_VALUE(UnsupportedProtocol);
AMREXPLORER_ASSERT_ERROR_VALUE(InvalidRequest);
AMREXPLORER_ASSERT_ERROR_VALUE(UnknownDataset);
AMREXPLORER_ASSERT_ERROR_VALUE(DatasetOpenFailure);
AMREXPLORER_ASSERT_ERROR_VALUE(Cancelled);
AMREXPLORER_ASSERT_ERROR_VALUE(CacheBudgetExceeded);
AMREXPLORER_ASSERT_ERROR_VALUE(ResourceLimitExceeded);
AMREXPLORER_ASSERT_ERROR_VALUE(OperationFailure);
AMREXPLORER_ASSERT_ERROR_VALUE(InternalServerError);
AMREXPLORER_ASSERT_ERROR_VALUE(Disconnected);
AMREXPLORER_ASSERT_ERROR_VALUE(Unauthorized);

#undef AMREXPLORER_ASSERT_ERROR_VALUE

void requireFinite(double value, const char* description)
{
    if (!std::isfinite(value)) {
        throw std::invalid_argument(description);
    }
}

template <typename Values>
void requireFiniteValues(const Values& values, const char* description)
{
    if (!std::all_of(values.begin(), values.end(),
            [](const auto value) { return std::isfinite(value); })) {
        throw std::invalid_argument(description);
    }
}

std::size_t checkedProduct(
    std::size_t left, std::size_t right, const char* description)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::invalid_argument(description);
    }
    return left * right;
}

template <typename Destination, typename Source>
void requireVectorSize(const Source& source, std::size_t size,
    const char* description)
{
    if (source.size() != size) {
        throw std::invalid_argument(description);
    }
    static_cast<void>(sizeof(Destination));
}

fb::SamplingPolicy toWireSampling(SamplingPolicy value)
{
    switch (value) {
    case SamplingPolicy::Nearest:
        return fb::SamplingPolicy::Nearest;
    case SamplingPolicy::PiecewiseConstant:
        return fb::SamplingPolicy::PiecewiseConstant;
    case SamplingPolicy::Linear:
        return fb::SamplingPolicy::Linear;
    }
    throw std::invalid_argument("unknown sampling policy");
}

SamplingPolicy fromWireSampling(fb::SamplingPolicy value)
{
    switch (value) {
    case fb::SamplingPolicy::Nearest:
        return SamplingPolicy::Nearest;
    case fb::SamplingPolicy::PiecewiseConstant:
        return SamplingPolicy::PiecewiseConstant;
    case fb::SamplingPolicy::Linear:
        return SamplingPolicy::Linear;
    default:
        throw std::invalid_argument("unknown wire sampling policy");
    }
}

fb::CompositionPolicy toWireComposition(CompositionPolicy value)
{
    switch (value) {
    case CompositionPolicy::FinestAvailable:
        return fb::CompositionPolicy::FinestAvailable;
    case CompositionPolicy::ExactLevel:
        return fb::CompositionPolicy::ExactLevel;
    }
    throw std::invalid_argument("unknown composition policy");
}

CompositionPolicy fromWireComposition(fb::CompositionPolicy value)
{
    switch (value) {
    case fb::CompositionPolicy::FinestAvailable:
        return CompositionPolicy::FinestAvailable;
    case fb::CompositionPolicy::ExactLevel:
        return CompositionPolicy::ExactLevel;
    default:
        throw std::invalid_argument("unknown wire composition policy");
    }
}

fb::Centering toWireCentering(Centering value)
{
    const auto raw = static_cast<std::uint8_t>(value);
    if (raw > static_cast<std::uint8_t>(Centering::Mixed)) {
        throw std::invalid_argument("unknown centering");
    }
    return static_cast<fb::Centering>(raw);
}

Centering fromWireCentering(fb::Centering value)
{
    const auto raw = static_cast<std::uint8_t>(value);
    if (raw > static_cast<std::uint8_t>(Centering::Mixed)) {
        throw std::invalid_argument("unknown wire centering");
    }
    return static_cast<Centering>(raw);
}

fb::ErrorCode toWireError(ErrorCode value)
{
    const auto raw = static_cast<std::uint16_t>(value);
    if (raw > static_cast<std::uint16_t>(ErrorCode::Unauthorized)) {
        throw std::invalid_argument("unknown error code");
    }
    return static_cast<fb::ErrorCode>(raw);
}

ErrorCode fromWireError(fb::ErrorCode value)
{
    const auto raw = static_cast<std::uint16_t>(value);
    if (raw > static_cast<std::uint16_t>(ErrorCode::Unauthorized)) {
        throw std::invalid_argument("unknown wire error code");
    }
    return static_cast<ErrorCode>(raw);
}

PayloadKind payloadKind(fb::Payload value)
{
    const auto raw = static_cast<std::uint8_t>(value);
    if (raw > static_cast<std::uint8_t>(PayloadKind::DirectoryListing)) {
        throw std::invalid_argument("unknown wire payload kind");
    }
    return static_cast<PayloadKind>(raw);
}

fb::RangeScope toWireRangeScope(RangeScope value)
{
    switch (value) {
    case RangeScope::File:
        return fb::RangeScope::File;
    case RangeScope::Level:
        return fb::RangeScope::Level;
    }
    throw std::invalid_argument("unknown range scope");
}

RangeScope fromWireRangeScope(fb::RangeScope value)
{
    switch (value) {
    case fb::RangeScope::File:
        return RangeScope::File;
    case fb::RangeScope::Level:
        return RangeScope::Level;
    default:
        throw std::invalid_argument("unknown wire range scope");
    }
}

void validateResultVectors(
    std::size_t expected, std::size_t values, std::size_t valid,
    std::size_t levels, const char* description)
{
    if (values != expected || valid != expected || levels != expected) {
        throw std::invalid_argument(description);
    }
}

} // namespace

std::unique_ptr<NativeEnvelope> decode(
    std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8
        || !fb::EnvelopeBufferHasIdentifier(bytes.data())) {
        throw std::invalid_argument(
            "wire payload does not have the AVR2 identifier");
    }
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (!fb::VerifyEnvelopeBuffer(verifier)) {
        throw std::invalid_argument("wire payload failed verification");
    }
    const auto* wireEnvelope = fb::GetEnvelope(bytes.data());
    if (wireEnvelope->request_id() == 0
        || wireEnvelope->payload_type() == fb::Payload::NONE) {
        throw std::invalid_argument("wire envelope is incomplete");
    }
    static_cast<void>(payloadKind(wireEnvelope->payload_type()));
    std::unique_ptr<NativeEnvelope> envelope(wireEnvelope->UnPack());
    if (!envelope) {
        throw std::invalid_argument("wire envelope could not be unpacked");
    }
    return envelope;
}

EnvelopeInfo inspect(const NativeEnvelope& envelope)
{
    if (envelope.request_id == 0 || envelope.payload.type == fb::Payload::NONE) {
        throw std::invalid_argument("wire envelope is incomplete");
    }
    return {envelope.protocol_major, envelope.protocol_minor_version,
        envelope.request_id, payloadKind(envelope.payload.type)};
}

std::unique_ptr<fb::Real3T> toWire(const Real3& value)
{
    auto wire = std::make_unique<fb::Real3T>();
    wire->values.assign(value.values.begin(), value.values.end());
    return wire;
}

std::unique_ptr<fb::Int3T> toWire(const Int3& value)
{
    auto wire = std::make_unique<fb::Int3T>();
    wire->values.assign(value.values.begin(), value.values.end());
    return wire;
}

std::unique_ptr<fb::RealBoxT> toWire(const RealBox& value)
{
    auto wire = std::make_unique<fb::RealBoxT>();
    wire->lower = toWire(value.lower);
    wire->upper = toWire(value.upper);
    return wire;
}

std::unique_ptr<fb::IntBoxT> toWire(const IntBox& value)
{
    auto wire = std::make_unique<fb::IntBoxT>();
    wire->lower = toWire(value.lower);
    wire->upper = toWire(value.upper);
    wire->centering = toWire(value.centering);
    return wire;
}

Real3 fromWire(const fb::Real3T* value)
{
    if (value == nullptr || value->values.size() != 3) {
        throw std::invalid_argument("wire Real3 must contain three values");
    }
    requireFiniteValues(value->values,
        "wire Real3 contains a non-finite value");
    Real3 result;
    std::copy(value->values.begin(), value->values.end(),
        result.values.begin());
    return result;
}

Int3 fromWire(const fb::Int3T* value)
{
    if (value == nullptr || value->values.size() != 3) {
        throw std::invalid_argument("wire Int3 must contain three values");
    }
    Int3 result;
    std::copy(value->values.begin(), value->values.end(),
        result.values.begin());
    return result;
}

RealBox fromWire(const fb::RealBoxT* value)
{
    if (value == nullptr) {
        throw std::invalid_argument("wire RealBox is missing");
    }
    RealBox result;
    result.lower = fromWire(value->lower.get());
    result.upper = fromWire(value->upper.get());
    return result;
}

IntBox fromWire(const fb::IntBoxT* value)
{
    if (value == nullptr) {
        throw std::invalid_argument("wire IntBox is missing");
    }
    IntBox result;
    result.lower = fromWire(value->lower.get());
    result.upper = fromWire(value->upper.get());
    if (value->centering) {
        result.centering = fromWire(value->centering.get());
    }
    return result;
}

std::unique_ptr<fb::CacheStateT> toWire(const CacheMetrics& value)
{
    auto wire = std::make_unique<fb::CacheStateT>();
    wire->budget_bytes = value.budgetBytes;
    wire->resident_bytes = value.residentBytes;
    wire->pinned_bytes = value.pinnedBytes;
    wire->hits = value.hits;
    wire->misses = value.misses;
    wire->evictions = value.evictions;
    wire->clears = value.clears;
    return wire;
}

CacheMetrics fromWire(const fb::CacheStateT* value)
{
    if (value == nullptr) {
        throw std::invalid_argument("wire cache state is missing");
    }
    return {value->budget_bytes, value->resident_bytes, value->pinned_bytes,
        value->hits, value->misses, value->evictions, value->clears};
}

fb::HelloRequestT toWire(const HelloRequestData& value)
{
    fb::HelloRequestT wire;
    wire.client_name = value.clientName;
    wire.software_version = value.softwareVersion;
    wire.minimum_minor_version = value.minimumMinorVersion;
    wire.maximum_minor_version = value.maximumMinorVersion;
    wire.maximum_frame_bytes = value.maximumFrameBytes;
    wire.session_token = value.sessionToken;
    wire.capabilities = value.capabilities;
    return wire;
}

HelloRequestData fromWire(const fb::HelloRequestT& value)
{
    HelloRequestData result;
    result.clientName = value.client_name;
    result.softwareVersion = value.software_version;
    result.minimumMinorVersion = value.minimum_minor_version;
    result.maximumMinorVersion = value.maximum_minor_version;
    result.maximumFrameBytes = value.maximum_frame_bytes;
    result.sessionToken = value.session_token;
    result.capabilities = value.capabilities;
    return result;
}

fb::HelloResponseT toWire(const HelloResponseData& value)
{
    fb::HelloResponseT wire;
    wire.server_name = value.serverName;
    wire.software_version = value.softwareVersion;
    wire.selected_minor_version = value.selectedMinorVersion;
    wire.maximum_frame_bytes = value.maximumFrameBytes;
    wire.maximum_datasets = value.maximumDatasets;
    wire.maximum_outstanding_requests = value.maximumOutstandingRequests;
    wire.worker_count = value.workerCount;
    wire.capabilities = value.capabilities;
    return wire;
}

HelloResponseData fromWire(const fb::HelloResponseT& value)
{
    HelloResponseData result;
    result.serverName = value.server_name;
    result.softwareVersion = value.software_version;
    result.selectedMinorVersion = value.selected_minor_version;
    result.maximumFrameBytes = value.maximum_frame_bytes;
    result.maximumDatasets = value.maximum_datasets;
    result.maximumOutstandingRequests = value.maximum_outstanding_requests;
    result.workerCount = value.worker_count;
    result.capabilities = value.capabilities;
    return result;
}

fb::OpenDatasetRequestT toWire(const OpenDatasetData& value)
{
    fb::OpenDatasetRequestT wire;
    wire.path = value.path;
    wire.cache_budget_bytes = value.cacheBudgetBytes;
    return wire;
}

OpenDatasetData fromWire(const fb::OpenDatasetRequestT& value)
{
    return {value.path, value.cache_budget_bytes};
}

fb::ListDirectoryRequestT toWireDirectoryRequest(const std::string& path)
{
    fb::ListDirectoryRequestT wire;
    wire.path = path;
    return wire;
}

fb::DirectoryListingT toWire(const RemoteDirectoryListing& value)
{
    fb::DirectoryListingT wire;
    wire.path = value.path;
    wire.parent_path = value.parentPath;
    wire.entries.reserve(value.entries.size());
    for (const auto& entry : value.entries) {
        auto converted = std::make_unique<fb::DirectoryEntryT>();
        converted->name = entry.name;
        converted->path = entry.path;
        converted->is_plotfile = entry.isPlotfile;
        wire.entries.push_back(std::move(converted));
    }
    return wire;
}

RemoteDirectoryListing fromWire(const fb::DirectoryListingT& value)
{
    RemoteDirectoryListing listing;
    listing.path = value.path;
    listing.parentPath = value.parent_path;
    listing.entries.reserve(value.entries.size());
    for (const auto& entry : value.entries) {
        if (entry == nullptr || entry->name.empty()
            || entry->name == "." || entry->name == ".."
            || entry->name.find('/') != std::string::npos
            || entry->name.find('\\') != std::string::npos) {
            throw std::invalid_argument(
                "directory listing contains an invalid entry");
        }
        if (entry->path.empty()) {
            throw std::invalid_argument(
                "directory listing contains an empty path");
        }
        listing.entries.push_back(
            {entry->name, entry->path, entry->is_plotfile});
    }
    return listing;
}

fb::DatasetOpenedT toWire(const OpenedDataset& value)
{
    fb::DatasetOpenedT wire;
    wire.dataset_id = value.id.value;
    wire.dimension = value.catalog.dimension;
    wire.finest_level = value.catalog.finestLevel;
    wire.is_fab = value.catalog.isFab;
    wire.has_physical_geometry = value.catalog.hasPhysicalGeometry;
    wire.time = value.catalog.time;
    wire.coordinate_system = value.catalog.coordinateSystem;
    wire.physical_domain = toWire(value.catalog.physicalDomain);
    for (const auto& field : value.catalog.fields) {
        auto converted = std::make_unique<fb::FieldCatalogT>();
        converted->name = field.name;
        converted->centering = toWireCentering(field.centering);
        converted->component_names = field.componentNames;
        wire.fields.push_back(std::move(converted));
    }
    for (const auto& level : value.catalog.levels) {
        auto converted = std::make_unique<fb::LevelCatalogT>();
        converted->level = level.level;
        converted->step = level.step;
        converted->domain = toWire(level.domain);
        converted->cell_size = toWire(level.cellSize);
        converted->index_origin = toWire(level.indexOrigin);
        converted->boxes.reserve(level.boxes.size());
        for (const auto& box : level.boxes) {
            converted->boxes.push_back(toWire(box));
        }
        wire.levels.push_back(std::move(converted));
    }
    for (const auto& species : value.particleSpecies) {
        auto converted = std::make_unique<fb::ParticleSpeciesCatalogT>();
        converted->name = species.name;
        converted->dimension = species.dimension;
        converted->real_component_count = species.realComponentCount;
        converted->int_component_count = species.intComponentCount;
        converted->particle_count = species.particleCount;
        converted->single_precision
            = species.precision == ParticleRealPrecision::Single;
        wire.particle_species.push_back(std::move(converted));
    }
    wire.file_range_available = value.fileRangeAvailable;
    wire.level_range_available = value.levelRangeAvailable;
    wire.metadata_metrics = std::make_unique<fb::MetadataReadMetricsT>();
    wire.metadata_metrics->files_read = value.metadataMetrics.filesRead;
    wire.metadata_metrics->bytes_read = value.metadataMetrics.bytesRead;
    wire.metadata_metrics->payload_files_read
        = value.metadataMetrics.payloadFilesRead;
    wire.metadata_metrics->payload_bytes_read
        = value.metadataMetrics.payloadBytesRead;
    wire.file_version = value.fileVersion;
    wire.cache = toWire(value.cache);
    return wire;
}

OpenedDataset fromWire(const fb::DatasetOpenedT& value)
{
    if (value.dimension < 1 || value.dimension > 3) {
        throw std::invalid_argument(
            "wire dataset dimension is outside [1, 3]");
    }
    if (value.finest_level < 0) {
        throw std::invalid_argument("wire finest level is negative");
    }
    const auto expectedLevelCount
        = static_cast<std::size_t>(value.finest_level) + std::size_t{1};
    if (value.levels.size() != expectedLevelCount) {
        throw std::invalid_argument("wire level catalog count is inconsistent");
    }
    const auto fieldCount = value.fields.size();
    const auto levelCount = value.levels.size();
    const auto expectedLevelRanges = checkedProduct(fieldCount, levelCount,
        "wire range-availability catalog size overflows");
    if (value.file_range_available.size() != fieldCount
        || value.level_range_available.size() != expectedLevelRanges) {
        throw std::invalid_argument(
            "wire range-availability catalog is inconsistent");
    }
    requireFinite(value.time, "wire dataset time is non-finite");
    static_cast<void>(fromWire(value.physical_domain.get()));
    for (const auto& field : value.fields) {
        if (!field) {
            throw std::invalid_argument("wire field catalog is missing");
        }
        static_cast<void>(fromWireCentering(field->centering));
    }
    for (const auto& level : value.levels) {
        if (!level) {
            throw std::invalid_argument("wire level catalog is missing");
        }
        static_cast<void>(fromWire(level->domain.get()));
        static_cast<void>(fromWire(level->cell_size.get()));
        static_cast<void>(fromWire(level->index_origin.get()));
        for (const auto& box : level->boxes) {
            if (!box) {
                throw std::invalid_argument("wire level box is missing");
            }
            static_cast<void>(fromWire(box.get()));
        }
    }
    for (const auto& species : value.particle_species) {
        if (!species) {
            throw std::invalid_argument("wire particle catalog is missing");
        }
    }
    if (!value.metadata_metrics) {
        throw std::invalid_argument("wire metadata metrics are missing");
    }
    static_cast<void>(fromWire(value.cache.get()));

    OpenedDataset result;
    result.id = DatasetId{value.dataset_id};
    result.catalog.dimension = value.dimension;
    result.catalog.finestLevel = value.finest_level;
    result.catalog.isFab = value.is_fab;
    result.catalog.hasPhysicalGeometry = value.has_physical_geometry;
    result.catalog.time = value.time;
    result.catalog.coordinateSystem = value.coordinate_system;
    result.catalog.physicalDomain = fromWire(value.physical_domain.get());
    for (const auto& field : value.fields) {
        if (!field) {
            throw std::invalid_argument("wire field catalog is missing");
        }
        result.catalog.fields.push_back(FieldMetadata{
            field->name, fromWireCentering(field->centering),
            field->component_names});
    }
    for (const auto& level : value.levels) {
        if (!level) {
            throw std::invalid_argument("wire level catalog is missing");
        }
        LevelMetadata converted;
        converted.level = level->level;
        converted.step = level->step;
        converted.domain = fromWire(level->domain.get());
        converted.cellSize = fromWire(level->cell_size.get());
        converted.indexOrigin = fromWire(level->index_origin.get());
        converted.boxes.reserve(level->boxes.size());
        for (const auto& box : level->boxes) {
            if (!box) {
                throw std::invalid_argument("wire level box is missing");
            }
            converted.boxes.push_back(fromWire(box.get()));
        }
        result.catalog.levels.push_back(std::move(converted));
    }
    for (const auto& species : value.particle_species) {
        if (!species) {
            throw std::invalid_argument("wire particle catalog is missing");
        }
        result.particleSpecies.push_back(ParticleSpeciesMetadata{
            species->name, species->dimension,
            species->real_component_count, species->int_component_count,
            species->particle_count,
            species->single_precision ? ParticleRealPrecision::Single
                                      : ParticleRealPrecision::Double});
    }
    result.fileRangeAvailable = value.file_range_available;
    result.levelRangeAvailable = value.level_range_available;
    result.metadataMetrics = {value.metadata_metrics->files_read,
        value.metadata_metrics->bytes_read,
        value.metadata_metrics->payload_files_read,
        value.metadata_metrics->payload_bytes_read};
    result.fileVersion = value.file_version;
    result.cache = fromWire(value.cache.get());
    // Structural checks above prove the wire is well formed; this proves the
    // catalog is a dataset. Both local readers throw on any validateMetadata
    // issue, so every catalog a server can serve has already satisfied this --
    // running it here makes the remote path reject exactly what a local open
    // would, instead of publishing a session whose box ordering, containment,
    // level numbering, or cell sizes are impossible.
    const auto issues = validateMetadata(result.catalog);
    if (!issues.empty()) {
        throw std::invalid_argument("wire dataset catalog is invalid at "
            + issues.front().path + ": " + issues.front().message);
    }
    for (const auto& species : result.particleSpecies) {
        if (species.name.empty()) {
            throw std::invalid_argument("wire particle species has no name");
        }
        if (species.dimension < 1 || species.dimension > 3) {
            throw std::invalid_argument(
                "wire particle species dimension is outside [1, 3]");
        }
        if (species.realComponentCount < 0
            || species.realComponentCount > maximumParticleComponents
            || species.intComponentCount < 0
            || species.intComponentCount > maximumParticleComponents) {
            throw std::invalid_argument(
                "wire particle species component count is outside its bounds");
        }
    }
    return result;
}

fb::SliceViewRequestT toWire(const SliceRequest& value)
{
    fb::SliceViewRequestT wire;
    wire.dataset_id = value.dataset.value;
    wire.field = value.field.value;
    wire.component = value.component;
    wire.normal_direction = value.normalDirection;
    wire.physical_position = value.physicalPosition;
    wire.visible_region = toWire(value.visibleRegion);
    wire.maximum_level = value.maximumLevel;
    wire.width = value.outputSize[0];
    wire.height = value.outputSize[1];
    wire.sampling = toWireSampling(value.sampling);
    wire.composition = toWireComposition(value.composition);
    wire.include_grid_boxes = value.includeGridBoxes;
    return wire;
}

SliceRequest fromWire(const fb::SliceViewRequestT& value)
{
    requireFinite(value.physical_position,
        "wire slice position is non-finite");
    const auto visibleRegion = fromWire(value.visible_region.get());
    const auto sampling = fromWireSampling(value.sampling);
    const auto composition = fromWireComposition(value.composition);
    SliceRequest result;
    result.dataset = DatasetId{value.dataset_id};
    result.field = FieldId{value.field};
    result.component = value.component;
    result.normalDirection = value.normal_direction;
    result.physicalPosition = value.physical_position;
    result.visibleRegion = visibleRegion;
    result.maximumLevel = value.maximum_level;
    result.outputSize = {value.width, value.height};
    result.sampling = sampling;
    result.composition = composition;
    result.includeGridBoxes = value.include_grid_boxes;
    return result;
}

fb::SliceViewResponseT toWire(
    const SliceQueryResult& value, const CacheMetrics& cache)
{
    fb::SliceViewResponseT wire;
    wire.width = value.plane.width;
    wire.height = value.plane.height;
    wire.physical_region = toWire(value.plane.physicalRegion);
    wire.values = value.plane.values;
    wire.valid = value.plane.valid;
    wire.source_level = value.plane.sourceLevel;
    wire.grid_boxes_included = value.gridBoxesIncluded;
    wire.grid_boxes_truncated = value.gridBoxesTruncated;
    wire.grid_boxes.reserve(value.gridBoxes.size());
    for (const auto& box : value.gridBoxes) {
        auto converted = std::make_unique<fb::SliceGridBoxT>();
        converted->level = box.level;
        converted->physical_region = toWire(box.physicalRegion);
        wire.grid_boxes.push_back(std::move(converted));
    }
    wire.candidate_blocks = value.metrics.candidateBlocks;
    wire.blocks_read = value.metrics.blocksRead;
    wire.cache_hits = value.metrics.cacheHits;
    wire.payload_bytes_read = value.metrics.payloadBytesRead;
    wire.cache = toWire(cache);
    return wire;
}

SliceQueryResult fromWire(const fb::SliceViewResponseT& value)
{
    if (value.width < 1 || value.height < 1) {
        throw std::invalid_argument("wire slice dimensions are invalid");
    }
    const auto expected = checkedProduct(static_cast<std::size_t>(value.width),
        static_cast<std::size_t>(value.height),
        "wire slice dimensions overflow");
    validateResultVectors(expected, value.values.size(), value.valid.size(),
        value.source_level.size(), "wire slice vectors are inconsistent");
    const auto physicalRegion = fromWire(value.physical_region.get());
    SliceQueryResult result;
    result.plane.width = value.width;
    result.plane.height = value.height;
    result.plane.physicalRegion = physicalRegion;
    result.plane.values = value.values;
    result.plane.valid = value.valid;
    result.plane.sourceLevel = value.source_level;
    result.gridBoxesIncluded = value.grid_boxes_included;
    result.gridBoxesTruncated = value.grid_boxes_truncated;
    result.gridBoxes.reserve(value.grid_boxes.size());
    for (const auto& box : value.grid_boxes) {
        if (!box) {
            throw std::invalid_argument("wire slice grid box is missing");
        }
        result.gridBoxes.push_back(
            SliceGridBox{box->level, fromWire(box->physical_region.get())});
    }
    result.metrics = {value.candidate_blocks, value.blocks_read,
        value.cache_hits, value.payload_bytes_read};
    return result;
}

fb::LineViewRequestT toWire(const LineViewRequest& value)
{
    fb::LineViewRequestT wire;
    wire.dataset_id = value.query.dataset.value;
    wire.field = value.query.field.value;
    wire.component = value.query.component;
    wire.axis = value.query.axis;
    Real3 fixed;
    fixed.values = value.query.fixedCoordinates;
    wire.fixed_coordinates = toWire(fixed);
    wire.maximum_level = value.query.maximumLevel;
    wire.composition = toWireComposition(value.query.composition);
    wire.has_region = value.query.region.has_value();
    if (value.query.region) {
        wire.region = toWire(*value.query.region);
    }
    wire.output_width = value.outputWidth;
    return wire;
}

LineViewRequest fromWire(const fb::LineViewRequestT& value)
{
    const auto fixedCoordinates = fromWire(value.fixed_coordinates.get()).values;
    const auto composition = fromWireComposition(value.composition);
    std::optional<RealBox> region;
    if (value.has_region) {
        region = fromWire(value.region.get());
    }
    LineViewRequest result;
    result.query.dataset = DatasetId{value.dataset_id};
    result.query.field = FieldId{value.field};
    result.query.component = value.component;
    result.query.axis = value.axis;
    result.query.fixedCoordinates = fixedCoordinates;
    result.query.maximumLevel = value.maximum_level;
    result.query.composition = composition;
    result.query.region = region;
    result.outputWidth = value.output_width;
    return result;
}

fb::LineViewResponseT toWire(
    const LineQueryResult& value, const CacheMetrics& cache)
{
    fb::LineViewResponseT wire;
    wire.axis = value.line.axis;
    wire.positions_are_indices = value.line.positionsAreIndices;
    wire.positions = value.line.positions;
    wire.values = value.line.values;
    wire.valid = value.line.valid;
    wire.source_level = value.line.sourceLevel;
    wire.candidate_blocks = value.metrics.candidateBlocks;
    wire.blocks_read = value.metrics.blocksRead;
    wire.cache_hits = value.metrics.cacheHits;
    wire.payload_bytes_read = value.metrics.payloadBytesRead;
    wire.cache = toWire(cache);
    return wire;
}

LineQueryResult fromWire(const fb::LineViewResponseT& value)
{
    validateResultVectors(value.positions.size(), value.values.size(),
        value.valid.size(), value.source_level.size(),
        "wire line vectors are inconsistent");
    requireFiniteValues(value.positions,
        "wire line positions contain a non-finite value");
    LineQueryResult result;
    result.line.axis = value.axis;
    result.line.positionsAreIndices = value.positions_are_indices;
    result.line.positions = value.positions;
    result.line.values = value.values;
    result.line.valid = value.valid;
    result.line.sourceLevel = value.source_level;
    result.metrics = {value.candidate_blocks, value.blocks_read,
        value.cache_hits, value.payload_bytes_read};
    return result;
}

fb::DatasetPageRequestT toWire(const DatasetPageRequest& value)
{
    fb::DatasetPageRequestT wire;
    wire.dataset_id = value.dataset.value;
    wire.field = value.field.value;
    wire.level = value.level;
    wire.region = toWire(value.region);
    wire.normal_axis = value.normalAxis;
    wire.slice_position = value.slicePosition;
    wire.maximum_extent = value.maximumExtent;
    return wire;
}

DatasetPageRequest fromWire(const fb::DatasetPageRequestT& value)
{
    requireFinite(value.slice_position,
        "wire dataset page slice position is non-finite");
    const auto region = fromWire(value.region.get());
    return {DatasetId{value.dataset_id}, FieldId{value.field}, value.level,
        region, value.normal_axis, value.slice_position, value.maximum_extent};
}

fb::DatasetPageResponseT toWire(
    const DatasetPage& value, const CacheMetrics& cache)
{
    fb::DatasetPageResponseT wire;
    wire.lower.assign(value.lower.begin(), value.lower.end());
    wire.upper.assign(value.upper.begin(), value.upper.end());
    wire.nx = value.nx;
    wire.ny = value.ny;
    wire.slice_index = value.sliceIndex;
    wire.values = value.values;
    wire.covered = value.covered;
    wire.minimum = value.minimum;
    wire.maximum = value.maximum;
    wire.has_finite_values = value.hasFiniteValues;
    wire.truncated_x = value.truncatedX;
    wire.truncated_y = value.truncatedY;
    wire.cache = toWire(cache);
    return wire;
}

DatasetPage fromWire(const fb::DatasetPageResponseT& value)
{
    requireVectorSize<int>(value.lower, 2,
        "wire dataset page lower bound is invalid");
    requireVectorSize<int>(value.upper, 2,
        "wire dataset page upper bound is invalid");
    if (value.nx < 0 || value.ny < 0
        || value.nx > datasetPageMaxExtent
        || value.ny > datasetPageMaxExtent) {
        throw std::invalid_argument("wire dataset page extent is invalid");
    }
    const auto expected = checkedProduct(static_cast<std::size_t>(value.nx),
        static_cast<std::size_t>(value.ny),
        "wire dataset page extent overflows");
    if (value.values.size() != expected || value.covered.size() != expected) {
        throw std::invalid_argument(
            "wire dataset page vectors are inconsistent");
    }
    if (value.has_finite_values) {
        requireFinite(value.minimum,
            "wire dataset page minimum is non-finite");
        requireFinite(value.maximum,
            "wire dataset page maximum is non-finite");
        if (value.minimum > value.maximum) {
            throw std::invalid_argument("wire dataset page range is invalid");
        }
    }
    DatasetPage result;
    std::copy(value.lower.begin(), value.lower.end(), result.lower.begin());
    std::copy(value.upper.begin(), value.upper.end(), result.upper.begin());
    result.nx = value.nx;
    result.ny = value.ny;
    result.sliceIndex = value.slice_index;
    result.values = value.values;
    result.covered = value.covered;
    result.minimum = value.minimum;
    result.maximum = value.maximum;
    result.hasFiniteValues = value.has_finite_values;
    result.truncatedX = value.truncated_x;
    result.truncatedY = value.truncated_y;
    return result;
}

fb::ParticleSampleRequestT toWire(DatasetId dataset,
    const std::string& species, double fraction, std::uint64_t seed)
{
    fb::ParticleSampleRequestT wire;
    wire.dataset_id = dataset.value;
    wire.species = species;
    wire.fraction = fraction;
    wire.seed = seed;
    return wire;
}

ParticleSampleRequestData fromWire(
    const fb::ParticleSampleRequestT& value)
{
    if (value.species.empty() || !(value.fraction > 0.0)
        || value.fraction > 1.0) {
        throw std::invalid_argument("wire particle sample request is invalid");
    }
    return {DatasetId{value.dataset_id}, value.species,
        value.fraction, value.seed};
}

fb::ParticleSampleResponseT toWire(
    const ParticleSample& value, const CacheMetrics& cache)
{
    fb::ParticleSampleResponseT wire;
    wire.species = std::make_unique<fb::ParticleSpeciesCatalogT>();
    wire.species->name = value.species.name;
    wire.species->dimension = value.species.dimension;
    wire.species->real_component_count = value.species.realComponentCount;
    wire.species->int_component_count = value.species.intComponentCount;
    wire.species->particle_count = value.species.particleCount;
    wire.species->single_precision
        = value.species.precision == ParticleRealPrecision::Single;
    wire.ids.reserve(value.points.size());
    wire.positions.reserve(value.points.size() * 3);
    for (const auto& point : value.points) {
        wire.ids.push_back(point.id);
        wire.positions.insert(wire.positions.end(),
            point.position.values.begin(), point.position.values.end());
    }
    wire.integer_bytes_read = value.io.integerBytesRead;
    wire.real_bytes_read = value.io.realBytesRead;
    wire.level_directories_scanned = value.io.levelDirectoriesScanned;
    wire.data_files_opened = value.io.dataFilesOpened;
    wire.cache = toWire(cache);
    return wire;
}

ParticleSample fromWire(const fb::ParticleSampleResponseT& value)
{
    // Divide rather than multiply: ids.size() * 3 is unreachable for a vector
    // that fits in memory, but the wire is not the place to rely on that.
    if (!value.species || value.positions.size() % 3 != 0
        || value.positions.size() / 3 != value.ids.size()) {
        throw std::invalid_argument(
            "wire particle sample vectors are inconsistent");
    }
    // NaN or infinity here would reach projection and scene-coordinate
    // arithmetic, where it silently poisons overlay geometry.
    requireFiniteValues(value.positions,
        "wire particle positions contain a non-finite value");
    ParticleSample result;
    result.species = {value.species->name, value.species->dimension,
        value.species->real_component_count,
        value.species->int_component_count,
        value.species->particle_count,
        value.species->single_precision ? ParticleRealPrecision::Single
                                        : ParticleRealPrecision::Double};
    result.points.reserve(value.ids.size());
    for (std::size_t index = 0; index < value.ids.size(); ++index) {
        ParticlePoint point;
        point.id = value.ids[index];
        std::copy_n(value.positions.begin()
                + static_cast<std::ptrdiff_t>(index * 3),
            3, point.position.values.begin());
        result.points.push_back(point);
    }
    result.io = {value.integer_bytes_read, value.real_bytes_read,
        value.level_directories_scanned, value.data_files_opened};
    return result;
}

fb::RangeRequestT toWire(DatasetId dataset, const RangeRequest& value)
{
    fb::RangeRequestT wire;
    wire.dataset_id = dataset.value;
    wire.field = value.field.value;
    wire.maximum_level = value.maximumLevel;
    wire.composition = toWireComposition(value.composition);
    wire.scope = toWireRangeScope(value.scope);
    return wire;
}

std::pair<DatasetId, RangeRequest> fromWire(
    const fb::RangeRequestT& value)
{
    RangeRequest request;
    request.field = FieldId{value.field};
    request.maximumLevel = value.maximum_level;
    request.composition = fromWireComposition(value.composition);
    request.scope = fromWireRangeScope(value.scope);
    return {DatasetId{value.dataset_id}, request};
}

fb::RangeResponseT toWire(
    const std::optional<ValueRange>& value, const CacheMetrics& cache)
{
    fb::RangeResponseT wire;
    wire.has_range = value.has_value();
    if (value) {
        wire.minimum = value->minimum;
        wire.maximum = value->maximum;
    }
    wire.cache = toWire(cache);
    return wire;
}

std::optional<ValueRange> fromWire(const fb::RangeResponseT& value)
{
    if (!value.has_range) {
        return std::nullopt;
    }
    requireFinite(value.minimum, "wire range minimum is non-finite");
    requireFinite(value.maximum, "wire range maximum is non-finite");
    if (value.minimum > value.maximum) {
        throw std::invalid_argument("wire range is invalid");
    }
    return ValueRange{value.minimum, value.maximum};
}

fb::ErrorResponseT toWire(const ErrorData& value)
{
    fb::ErrorResponseT wire;
    wire.code = toWireError(value.code);
    wire.message = value.message;
    return wire;
}

ErrorData fromWire(const fb::ErrorResponseT& value)
{
    return {fromWireError(value.code), value.message};
}

} // namespace amrvis::remote::codec
