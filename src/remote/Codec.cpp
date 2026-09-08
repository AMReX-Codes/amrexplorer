#include "Codec.hpp"

#include "amrexplorer_wire_bfbs_generated.h"

#include <amrexplorer/expression/Expression.hpp>

#include <flatbuffers/reflection.h>
#include <flatbuffers/verifier.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
AMREXPLORER_ASSERT_PAYLOAD_VALUE(RenderedFrameRequest, RenderedFrameRequest);
AMREXPLORER_ASSERT_PAYLOAD_VALUE(RenderedFrameResponse, RenderedFrameResponse);

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

fb::ColorScale toWireColorScale(ColorScale value)
{
    return static_cast<fb::ColorScale>(static_cast<std::uint8_t>(value));
}

ColorScale fromWireColorScale(fb::ColorScale value)
{
    const auto raw = static_cast<std::uint8_t>(value);
    if (raw > static_cast<std::uint8_t>(ColorScale::SymLogarithmic)) {
        throw std::invalid_argument("unknown wire color scale");
    }
    return static_cast<ColorScale>(raw);
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
    if (raw > static_cast<std::uint8_t>(PayloadKind::RenderedFrameResponse)) {
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

// flatbuffers' Verifier checks the alignment of table scalars and of a
// vector's 4-byte length prefix, but not of the vector's elements, so a
// hostile buffer can place a [double] or [ulong] vector at a 4-byte offset
// and still verify; UnPack then reads misaligned 8-byte scalars, which is UB
// (and a fault on strict-alignment targets). A conforming builder aligns
// every vector's elements naturally relative to the buffer start, so anything
// else is malformed. This walks a verified buffer with the schema's own
// reflection data and rejects it -- generic over the schema, so new vector
// fields are covered without a hand-kept list. Everything dereferenced here
// was bounds-checked by the Verifier against the same schema.
class VectorAlignmentWalk
{
public:
    VectorAlignmentWalk(
        const reflection::Schema& schema, const std::uint8_t* base)
        : schema_(schema), base_(base)
    {}

    bool aligned(
        const reflection::Object& object,
        const flatbuffers::Table& table) const
    {
        for (const auto* field : *object.fields()) {
            const auto& type = *field->type();
            switch (type.base_type()) {
            case reflection::Obj: {
                // Structs are stored inline and alignment-checked by the
                // Verifier; only tables have contents to walk.
                const auto& child = objectAt(type.index());
                const auto* sub = child.is_struct()
                    ? nullptr : flatbuffers::GetFieldT(table, *field);
                if (sub && !aligned(child, *sub)) {
                    return false;
                }
                break;
            }
            case reflection::Union: {
                const auto* sub = flatbuffers::GetFieldT(table, *field);
                if (sub && !alignedUnion(object, *field, table, *sub)) {
                    return false;
                }
                break;
            }
            case reflection::Vector:
                if (!alignedVector(*field, table)) {
                    return false;
                }
                break;
            default:
                // Scalars and strings: the Verifier already checks them.
                break;
            }
        }
        return true;
    }

private:
    bool alignedUnion(
        const reflection::Object& parent, const reflection::Field& field,
        const flatbuffers::Table& table, const flatbuffers::Table& sub) const
    {
        const auto* typeField = parent.fields()->LookupByKey(
            (field.name()->str() + flatbuffers::UnionTypeFieldSuffix())
                .c_str());
        if (!typeField) {
            return false;
        }
        const auto* member = schema_.enums()
            ->Get(static_cast<flatbuffers::uoffset_t>(field.type()->index()))
            ->values()->LookupByKey(
                flatbuffers::GetFieldI<std::uint8_t>(table, *typeField));
        // NONE and members this build does not know have no table to walk;
        // decode rejects both before UnPack. Struct members are stored
        // inline and alignment-checked by the Verifier.
        if (!member || !member->union_type()
            || member->union_type()->base_type() != reflection::Obj
            || objectAt(member->union_type()->index()).is_struct()) {
            return true;
        }
        return aligned(objectAt(member->union_type()->index()), sub);
    }

    bool alignedVector(
        const reflection::Field& field, const flatbuffers::Table& table) const
    {
        const auto* vec = flatbuffers::GetFieldAnyV(table, field);
        if (!vec || vec->size() == 0) {
            return true;
        }
        const auto& type = *field.type();
        const reflection::Object* element = type.element() == reflection::Obj
            ? &objectAt(type.index()) : nullptr;
        const std::size_t align = element
            ? (element->is_struct()
                ? static_cast<std::size_t>(element->minalign())
                : sizeof(flatbuffers::uoffset_t))
            : flatbuffers::GetTypeSize(type.element());
        if (align > 1
            && static_cast<std::size_t>(vec->Data() - base_) % align != 0) {
            return false;
        }
        if (element && !element->is_struct()) {
            const auto* tables = flatbuffers::GetFieldV<
                flatbuffers::Offset<flatbuffers::Table>>(table, field);
            for (const auto* sub : *tables) {
                if (!aligned(*element, *sub)) {
                    return false;
                }
            }
        }
        return true;
    }

    const reflection::Object& objectAt(std::int32_t index) const
    {
        return *schema_.objects()->Get(
            static_cast<flatbuffers::uoffset_t>(index));
    }

    const reflection::Schema& schema_;
    const std::uint8_t* base_;
};

bool vectorsAligned(std::span<const std::uint8_t> bytes)
{
    static const reflection::Schema& schema
        = *reflection::GetSchema(fb::EnvelopeBinarySchema::data());
    return VectorAlignmentWalk(schema, bytes.data())
        .aligned(*schema.root_table(), *flatbuffers::GetAnyRoot(bytes.data()));
}

// A skip's reason is written by installation, which quotes a symbol out of the
// expression it could not resolve -- so it runs to the expression's own bound
// plus the words around it, which is longer than the bound fromWire enforces
// on the way back in. Bounded here rather than left to disagree: a decoder
// that refused its own encoder's output would take the throw inside
// refusingInvalidResponses and close the connection, losing every other
// dataset on it over a reply the server was right to send.
std::string boundedReason(const std::string& reason)
{
    constexpr std::string_view continued = "...";
    // The bound is expression/Expression.hpp's, which is about expression
    // source rather than about messages -- nothing here owns it. Were it ever
    // set below the marker, the subtraction would wrap and the index below
    // would read off the end of the string before substr could clamp it.
    static_assert(maximumExpressionBytes > continued.size());
    if (reason.size() <= maximumExpressionBytes) {
        return reason;
    }
    auto kept = maximumExpressionBytes - continued.size();
    // Not mid-character: splitting a UTF-8 sequence would put bytes on the
    // wire that no longer read as text in the tooltip this ends up in.
    while (kept > 0
        && (static_cast<unsigned char>(reason[kept]) & 0xC0U) == 0x80U) {
        --kept;
    }
    return reason.substr(0, kept) + std::string(continued);
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
    if (!vectorsAligned(bytes)) {
        throw std::invalid_argument("wire payload has misaligned vector data");
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

bool isValidDirectoryEntryName(std::string_view name) noexcept
{
    return !name.empty() && name != "." && name != ".."
        && name.find('/') == std::string_view::npos
        && name.find(char{}) == std::string_view::npos;
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
    wire.truncated = value.truncated;
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
    if (value.entries.size() > maximumDirectoryEntries) {
        throw std::invalid_argument("directory listing exceeds the entry cap");
    }
    if (value.path.empty() || value.parent_path.empty()) {
        throw std::invalid_argument("directory listing names no directory");
    }
    RemoteDirectoryListing listing;
    listing.path = value.path;
    listing.parentPath = value.parent_path;
    listing.truncated = value.truncated;
    listing.entries.reserve(value.entries.size());
    for (const auto& entry : value.entries) {
        if (entry == nullptr || !isValidDirectoryEntryName(entry->name)
            || entry->path.empty()) {
            throw std::invalid_argument(
                "directory listing contains an invalid entry");
        }
        listing.entries.push_back(
            {entry->name, entry->path, entry->is_plotfile});
    }
    return listing;
}

fb::OpenDatasetRequestT toWire(const OpenDatasetData& value)
{
    fb::OpenDatasetRequestT wire;
    wire.path = value.path;
    wire.cache_budget_bytes = value.cacheBudgetBytes;
    wire.derived_fields.reserve(value.derivedFields.size());
    for (const auto& definition : value.derivedFields) {
        auto entry = std::make_unique<fb::DerivedFieldDefinitionT>();
        entry->name = definition.name;
        entry->expression = definition.expression;
        wire.derived_fields.push_back(std::move(entry));
    }
    return wire;
}

OpenDatasetData fromWire(const fb::OpenDatasetRequestT& value)
{
    // Only what a skip cannot express is refused here. The server installs
    // this list under DerivedFieldPolicy::Skip, so a definition it cannot
    // resolve comes back as one greyed row instead of an open that failed, and
    // a check repeated here would turn that row into a refusal the same list
    // never gets from a local dataset.
    //
    // These two are the exceptions, because the reply has no way to carry
    // them: a skip is reported by name, so a nameless definition cannot be
    // named back, and the reply's vector of skips is bounded by this same
    // maximumDerivedFieldCount, so a list past the cap could only be answered
    // with a reply this decoder would itself reject.
    if (value.derived_fields.size() > maximumDerivedFieldCount) {
        throw std::invalid_argument(
            "wire open request carries too many derived-field definitions");
    }
    OpenDatasetData result;
    result.path = value.path;
    result.cacheBudgetBytes = value.cache_budget_bytes;
    result.derivedFields.reserve(value.derived_fields.size());
    for (const auto& entry : value.derived_fields) {
        if (!entry) {
            throw std::invalid_argument(
                "wire derived-field definition is missing");
        }
        if (entry->name.empty()) {
            throw std::invalid_argument(
                "wire derived-field definition has no name");
        }
        // The expression's length is not checked: CompiledExpression::compile
        // refuses one past maximumExpressionBytes before it parses anything, so
        // installation already skips it with a bounded reason, which is what a
        // local dataset does with the same definition.
        result.derivedFields.push_back(
            DerivedFieldDefinition{entry->name, entry->expression});
    }
    return result;
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
    wire.derived_field_count = value.derivedFieldCount;
    wire.derived_field_skips.reserve(value.derivedFieldSkips.size());
    for (const auto& skip : value.derivedFieldSkips) {
        auto converted = std::make_unique<fb::DerivedFieldSkipT>();
        converted->definition_index
            = static_cast<std::uint32_t>(skip.definitionIndex);
        converted->name = skip.name;
        converted->reason = boundedReason(skip.reason);
        wire.derived_field_skips.push_back(std::move(converted));
    }
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
    // The derived fields are the tail of the catalog, so a count past its
    // length would leave the client splitting the list at an index that is not
    // in it. What the count cannot be checked against here is the number of
    // definitions the client sent -- the decoder does not know it -- which is
    // what validateSessionOpenedDerivedFields is for.
    result.derivedFieldCount = value.derived_field_count;
    if (result.derivedFieldCount > result.catalog.fields.size()) {
        throw std::invalid_argument(
            "wire dataset catalog claims more derived fields than it has "
            "fields");
    }
    if (value.derived_field_skips.size() > maximumDerivedFieldCount) {
        throw std::invalid_argument(
            "wire dataset catalog carries too many derived-field skips");
    }
    result.derivedFieldSkips.reserve(value.derived_field_skips.size());
    for (const auto& skip : value.derived_field_skips) {
        if (!skip) {
            throw std::invalid_argument(
                "wire derived-field skip is missing");
        }
        if (skip->name.empty()) {
            throw std::invalid_argument(
                "wire derived-field skip has no name");
        }
        // Clamped rather than refused. A reason over the bound is what a
        // *correct* peer produces -- installation quotes the symbol it could
        // not resolve, so the reason runs to the expression's own bound plus
        // the words around it -- and protocol 1.4 cannot tell a peer that
        // bounds it from one that does not. This decode runs inside
        // refusingInvalidResponses, where a throw closes the connection and
        // takes every other dataset on it, so display-only text must not be
        // able to do that. Our encoder bounds what we send; this is what
        // makes a peer built from any other 1.4 commit safe to talk to.
        result.derivedFieldSkips.push_back(
            DerivedFieldSkip{skip->definition_index, skip->name,
                boundedReason(skip->reason)});
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

fb::RenderedFrameRequestT toWire(const VolumeRenderRequest& value)
{
    fb::RenderedFrameRequestT wire;
    wire.dataset_id = value.dataset.value;
    wire.field = value.field.value;
    wire.component = value.component;
    wire.maximum_level = value.maximumLevel;
    wire.composition = toWireComposition(value.composition);
    wire.region = toWire(value.region);
    wire.azimuth = value.camera.azimuth;
    wire.elevation = value.camera.elevation;
    wire.zoom = value.camera.zoom;
    wire.width = value.outputSize[0];
    wire.height = value.outputSize[1];
    wire.has_range = value.range.has_value();
    wire.minimum = value.range ? value.range->minimum : 0.0;
    wire.maximum = value.range ? value.range->maximum : 0.0;
    wire.range_logarithmic = value.range && value.range->scale.scale == ColorScale::Logarithmic;
    wire.visible_logarithmic = value.scale.scale == ColorScale::Logarithmic;
    const auto rangeScale = value.range ? value.range->scale : ColorScaleConfig{};
    const auto visibleScale = value.scale;
    wire.range_scale = toWireColorScale(rangeScale.scale);
    wire.visible_scale = toWireColorScale(visibleScale.scale);
    wire.range_linear_threshold = rangeScale.linearThreshold;
    wire.visible_linear_threshold = visibleScale.linearThreshold;
    wire.transfer_colors = value.transfer.colors;
    wire.transfer_opacities = value.transfer.opacities;
    wire.samples_per_voxel = value.samplesPerVoxel;
    wire.maximum_voxels = value.maximumVoxels;
    wire.sampling = toWireSampling(value.sampling);
    return wire;
}

VolumeRenderRequest fromWire(const fb::RenderedFrameRequestT& value)
{
    requireFinite(value.azimuth, "wire volume camera azimuth is non-finite");
    requireFinite(value.elevation, "wire volume camera elevation is non-finite");
    requireFinite(value.zoom, "wire volume camera zoom is non-finite");
    if (value.has_range) {
        requireFinite(value.minimum, "wire volume range minimum is non-finite");
        requireFinite(value.maximum, "wire volume range maximum is non-finite");
    }
    if (value.transfer_colors.size() != value.transfer_opacities.size()
        || value.transfer_colors.size() > maxVolumeTransferEntries) {
        throw std::invalid_argument("wire volume transfer function is malformed");
    }
    requireFiniteValues(value.transfer_opacities,
        "wire volume transfer opacities are non-finite");
    const auto region = fromWire(value.region.get());
    const auto composition = fromWireComposition(value.composition);
    VolumeRenderRequest result;
    result.dataset = DatasetId{value.dataset_id};
    result.field = FieldId{value.field};
    result.component = value.component;
    result.maximumLevel = value.maximum_level;
    result.composition = composition;
    result.region = region;
    result.camera = {value.azimuth, value.elevation, value.zoom};
    result.outputSize = {value.width, value.height};
    if (value.has_range) {
        auto scale = ColorScaleConfig{fromWireColorScale(value.range_scale),
            value.range_linear_threshold};
        if (scale.scale == ColorScale::Linear && value.range_logarithmic)
            scale.scale = ColorScale::Logarithmic;
        result.range = VolumeRange{value.minimum, value.maximum, scale};
    }
    auto visibleScale = ColorScaleConfig{fromWireColorScale(value.visible_scale),
        value.visible_linear_threshold};
    if (visibleScale.scale == ColorScale::Linear && value.visible_logarithmic)
        visibleScale.scale = ColorScale::Logarithmic;
    result.scale = visibleScale;
    result.transfer.colors = value.transfer_colors;
    result.transfer.opacities = value.transfer_opacities;
    result.samplesPerVoxel = value.samples_per_voxel;
    result.maximumVoxels = value.maximum_voxels;
    result.sampling = fromWireSampling(value.sampling);
    return result;
}

fb::RenderedFrameResponseT toWire(
    VolumeFrame value, const CacheMetrics& cache)
{
    fb::RenderedFrameResponseT wire;
    wire.width = value.width;
    wire.height = value.height;
    // Moved, not copied: a frame at the output cap carries 67 MiB of pixels,
    // and the builder and the encoded buffer each hold one of their own while
    // this is alive.
    wire.pixels = std::move(value.pixels);
    wire.used_minimum = value.usedRange.minimum;
    wire.used_maximum = value.usedRange.maximum;
    wire.used_logarithmic = value.usedRange.scale.scale == ColorScale::Logarithmic;
    const auto usedScale = value.usedRange.scale;
    wire.used_scale = toWireColorScale(usedScale.scale);
    wire.used_linear_threshold = usedScale.linearThreshold;
    wire.grid_dims.assign(value.metrics.gridDims.begin(), value.metrics.gridDims.end());
    wire.covered_voxels = value.metrics.coveredVoxels;
    wire.sampled_maximum_level = value.metrics.sampledMaximumLevel;
    wire.grid_from_cache = value.metrics.gridFromCache;
    wire.sample_microseconds = value.metrics.sampleMicroseconds;
    wire.render_microseconds = value.metrics.renderMicroseconds;
    wire.candidate_blocks = value.metrics.candidateBlocks;
    wire.blocks_read = value.metrics.blocksRead;
    wire.cache_hits = value.metrics.cacheHits;
    wire.payload_bytes_read = value.metrics.payloadBytesRead;
    wire.cache_fallback_from_level = value.cacheFallbackFromLevel;
    wire.cache_fallback_to_level = value.cacheFallbackToLevel;
    wire.cache = toWire(cache);
    return wire;
}

VolumeFrame fromWire(const fb::RenderedFrameResponseT& value)
{
    if (value.width < 1 || value.height < 1) {
        throw std::invalid_argument("wire volume frame dimensions are invalid");
    }
    const auto expected = checkedProduct(static_cast<std::size_t>(value.width),
        static_cast<std::size_t>(value.height),
        "wire volume frame dimensions overflow");
    if (value.pixels.size() != expected) {
        throw std::invalid_argument("wire volume frame pixels do not match its size");
    }
    requireFinite(value.used_minimum, "wire volume range minimum is non-finite");
    requireFinite(value.used_maximum, "wire volume range maximum is non-finite");
    if (value.grid_dims.size() != 3) {
        throw std::invalid_argument("wire volume grid dimensions are malformed");
    }
    VolumeFrame result;
    result.width = value.width;
    result.height = value.height;
    result.pixels = value.pixels;
    auto usedScale = ColorScaleConfig{fromWireColorScale(value.used_scale),
        value.used_linear_threshold};
    if (usedScale.scale == ColorScale::Linear && value.used_logarithmic)
        usedScale.scale = ColorScale::Logarithmic;
    result.usedRange = {value.used_minimum, value.used_maximum, usedScale};
    result.metrics.gridDims = {value.grid_dims[0], value.grid_dims[1], value.grid_dims[2]};
    result.metrics.coveredVoxels = value.covered_voxels;
    result.metrics.sampledMaximumLevel = value.sampled_maximum_level;
    result.metrics.gridFromCache = value.grid_from_cache;
    result.metrics.sampleMicroseconds = value.sample_microseconds;
    result.metrics.renderMicroseconds = value.render_microseconds;
    result.metrics.candidateBlocks = value.candidate_blocks;
    result.metrics.blocksRead = value.blocks_read;
    result.metrics.cacheHits = value.cache_hits;
    result.metrics.payloadBytesRead = value.payload_bytes_read;
    result.cacheFallbackFromLevel = value.cache_fallback_from_level;
    result.cacheFallbackToLevel = value.cache_fallback_to_level;
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
