// Property/fuzz harness for the remote wire trust boundary. Contract:
// codec::decode of arbitrary or near-valid bytes, inspect of the decoded
// envelope, and every payload-matching fromWire converter (where the hostile
// payload *contents* are validated -- vector length agreement, finite values,
// enum ranges) yield a std::exception on rejection (the type production
// catches, see Server.cpp/Connection.cpp) and never crash, read out of
// bounds, or let a non-std::exception escape; a decoded envelope is
// inspectable and its summary agrees with it; an accepted payload satisfies
// what its converter claims to have validated (checkConverted below). Run
// under the qt-sanitizers preset to catch UB/OOB (the plain sanitizers preset
// builds without remote support, so this target does not exist there).
// Deterministic and bounded so it runs as a normal ctest; configure with
// -DAMREXPLORER_LIBFUZZER=ON (Clang), together with
// AMREXPLORER_ENABLE_SANITIZERS=ON so the fuzzer sees UB/OOB and not only
// hard crashes, to build it instead as a coverage-guided libFuzzer driver.
#include "fuzz_util.hpp"

#include "Codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

namespace codec = amrvis::remote::codec;
namespace fb = codec::fb;

using amrvis::fuzz::fail;

bool finite(double value)
{
    return std::isfinite(value);
}

bool finite(const amrvis::Real3& value)
{
    return std::all_of(value.values.begin(), value.values.end(),
        [](double component) { return std::isfinite(component); });
}

bool finite(const amrvis::RealBox& box)
{
    return finite(box.lower) && finite(box.upper);
}

// Bitwise equality: sample values may legitimately be NaN, which == denies.
bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size()
        && (a.empty()
            || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Postconditions of an accepted payload: what its fromWire converter claims
// to have validated (vector-length agreement, finite values, enum ranges,
// well-formed ranges) and that what it copies is the wire's own data -- so a
// converter that stops validating, or transposes, fails here instead of
// passing as "no crash".
void checkConverted(
    const fb::HelloRequestT& wire, const amrvis::remote::HelloRequestData& result)
{
    if (result.clientName != wire.client_name
        || result.sessionToken != wire.session_token
        || result.maximumFrameBytes != wire.maximum_frame_bytes
        || result.capabilities != wire.capabilities) {
        fail("HelloRequest did not convert faithfully");
    }
}

void checkConverted(const fb::HelloResponseT& wire,
    const amrvis::remote::HelloResponseData& result)
{
    if (result.serverName != wire.server_name
        || result.maximumFrameBytes != wire.maximum_frame_bytes
        || result.capabilities != wire.capabilities) {
        fail("HelloResponse did not convert faithfully");
    }
}

void checkConverted(const fb::OpenDatasetRequestT& wire,
    const amrvis::remote::OpenDatasetData& result)
{
    if (result.path != wire.path
        || result.cacheBudgetBytes != wire.cache_budget_bytes
        || result.derivedFields.size() != wire.derived_fields.size()) {
        fail("OpenDatasetRequest did not convert faithfully");
    }
    for (std::size_t index = 0; index < result.derivedFields.size(); ++index) {
        const auto& entry = wire.derived_fields[index];
        if (!entry || result.derivedFields[index].name != entry->name
            || result.derivedFields[index].expression != entry->expression) {
            fail("OpenDatasetRequest derived field did not convert "
                 "faithfully");
        }
    }
}

void checkConverted(
    const fb::DatasetOpenedT& wire, const amrvis::remote::OpenedDataset& result)
{
    const auto& catalog = result.catalog;
    const auto levels = catalog.levels.size();
    if (catalog.dimension < 1 || catalog.dimension > 3
        || catalog.finestLevel < 0
        || levels != static_cast<std::size_t>(catalog.finestLevel) + 1
        || levels != wire.levels.size()
        || catalog.fields.size() != wire.fields.size()
        || result.particleSpecies.size() != wire.particle_species.size()
        || result.fileRangeAvailable.size() != catalog.fields.size()
        || result.levelRangeAvailable.size() != catalog.fields.size() * levels
        || !finite(catalog.time) || !finite(catalog.physicalDomain)) {
        fail("DatasetOpened converter accepted an inconsistent catalog");
    }
    for (std::size_t i = 0; i < levels; ++i) {
        const auto& level = catalog.levels[i];
        if (level.boxes.size() != wire.levels[i]->boxes.size()
            || !finite(level.cellSize) || !finite(level.indexOrigin)) {
            fail("DatasetOpened converter accepted an inconsistent level");
        }
    }
    // The 1.4 tail, checked like the request's definitions are. This compares
    // the buffer against what fromWire made of it, so it is the decode that is
    // pinned: one that dropped the count, or that transposed a skip's name and
    // reason, would otherwise pass every iteration.
    if (result.derivedFieldCount != wire.derived_field_count
        || result.derivedFieldSkips.size() != wire.derived_field_skips.size()) {
        fail("DatasetOpened derived fields did not convert faithfully");
    }
    for (std::size_t i = 0; i < result.derivedFieldSkips.size(); ++i) {
        const auto& entry = wire.derived_field_skips[i];
        const auto& skip = result.derivedFieldSkips[i];
        if (!entry) {
            fail("DatasetOpened derived-field skip did not convert "
                 "faithfully");
        }
        // Equality unless the decoder was entitled to clamp: an over-long
        // reason comes back at the bound, and that is the only change it may
        // make to one.
        const auto reasonKept
            = entry->reason.size() <= amrvis::maximumExpressionBytes
            ? skip.reason == entry->reason
            : skip.reason.size() == amrvis::maximumExpressionBytes;
        if (skip.name != entry->name
            || skip.definitionIndex != entry->definition_index || !reasonKept) {
            fail("DatasetOpened derived-field skip did not convert "
                 "faithfully");
        }
    }
}

void checkConverted(
    const fb::SliceViewRequestT& wire, const amrvis::SliceRequest& result)
{
    if (!finite(result.visibleRegion) || !finite(result.physicalPosition)
        || result.outputSize != std::array{wire.width, wire.height}
        || result.dataset.value != wire.dataset_id) {
        fail("SliceViewRequest converter accepted a bad request");
    }
}

void checkConverted(
    const fb::SliceViewResponseT& wire, const amrvis::SliceQueryResult& result)
{
    const auto& plane = result.plane;
    if (plane.width < 1 || plane.height < 1) {
        fail("SliceViewResponse converter accepted bad dimensions");
    }
    const auto expected = static_cast<std::size_t>(plane.width)
        * static_cast<std::size_t>(plane.height);
    if (plane.values.size() != expected || plane.valid.size() != expected
        || plane.sourceLevel.size() != expected
        || !sameBits(plane.values, wire.values)
        || !finite(plane.physicalRegion)
        || result.gridBoxes.size() != wire.grid_boxes.size()) {
        fail("SliceViewResponse converter accepted inconsistent vectors");
    }
    for (const auto& box : result.gridBoxes) {
        if (!finite(box.physicalRegion)) {
            fail("SliceViewResponse converter accepted a non-finite grid box");
        }
    }
}

void checkConverted(const fb::RenderedFrameRequestT& wire,
    const amrvis::VolumeRenderRequest& result)
{
    // fromWire promises finite camera and range values and a consistent,
    // finite, bounded transfer function; the enums round-trip. Converted once
    // -- each call rebuilds the transfer vectors, and this runs every
    // iteration that reaches it.
    const auto roundTripped = codec::toWire(result);
    if (!finite(result.camera.azimuth) || !finite(result.camera.elevation)
        || !finite(result.camera.zoom) || !finite(result.region)
        || result.range.has_value() != wire.has_range
        || (result.range
            && (!finite(result.range->minimum) || !finite(result.range->maximum)))
        || result.transfer.colors.size() != result.transfer.opacities.size()
        || result.transfer.colors.size() > amrvis::maxVolumeTransferEntries
        || !std::all_of(result.transfer.opacities.begin(),
            result.transfer.opacities.end(),
            [](float opacity) { return std::isfinite(opacity); })
        || roundTripped.composition != wire.composition
        // The same round-trip check for the second enum on this table: a
        // transposed mapping converts cleanly in both directions and shows up
        // only against a peer that disagrees, which is what the composition
        // check above exists to catch.
        || roundTripped.sampling != wire.sampling
        || result.outputSize[0] != wire.width || result.outputSize[1] != wire.height) {
        fail("RenderedFrameRequest converter accepted a bad request");
    }
}

void checkConverted(
    const fb::RenderedFrameResponseT& wire, const amrvis::VolumeFrame& result)
{
    if (result.width < 1 || result.height < 1
        || result.pixels.size() != static_cast<std::size_t>(result.width)
                * static_cast<std::size_t>(result.height)
        || result.pixels != wire.pixels
        || !finite(result.usedRange.minimum) || !finite(result.usedRange.maximum)
        || wire.grid_dims.size() != 3
        || result.metrics.gridDims[0] != wire.grid_dims[0]
        || result.metrics.gridDims[1] != wire.grid_dims[1]
        || result.metrics.gridDims[2] != wire.grid_dims[2]) {
        fail("RenderedFrameResponse converter accepted an inconsistent frame");
    }
}

void checkConverted(
    const fb::LineViewRequestT& wire, const amrvis::LineViewRequest& result)
{
    if (!finite(amrvis::Real3{result.query.fixedCoordinates})
        || result.query.region.has_value() != wire.has_region
        || (result.query.region && !finite(*result.query.region))) {
        fail("LineViewRequest converter accepted a bad request");
    }
}

void checkConverted(
    const fb::LineViewResponseT& wire, const amrvis::LineQueryResult& result)
{
    const auto& line = result.line;
    const auto expected = line.positions.size();
    if (line.values.size() != expected || line.valid.size() != expected
        || line.sourceLevel.size() != expected
        || line.positions != wire.positions
        || !std::all_of(line.positions.begin(), line.positions.end(),
            [](double position) { return std::isfinite(position); })) {
        fail("LineViewResponse converter accepted inconsistent vectors");
    }
}

void checkConverted(const fb::DirectoryListingT& wire,
    const amrvis::remote::RemoteDirectoryListing& result)
{
    if (result.path != wire.path || result.parentPath != wire.parent_path
        || result.truncated != wire.truncated
        || result.entries.size() != wire.entries.size()
        || result.entries.size() > amrvis::remote::maximumDirectoryEntries) {
        fail("DirectoryListing converter did not carry the listing over");
    }
    for (std::size_t index = 0; index < result.entries.size(); ++index) {
        const auto& entry = result.entries[index];
        const auto& wireEntry = *wire.entries[index];
        if (entry.name != wireEntry.name || entry.path != wireEntry.path
            || entry.isPlotfile != wireEntry.is_plotfile) {
            fail("DirectoryListing converter did not carry an entry over");
        }
        // The converter's own predicate, so its removal from fromWire shows
        // here, and the properties spelled out, so its weakening does too:
        // one path component -- non-empty, not "." or "..", no '/', no NUL
        // -- with a non-empty path.
        if (!codec::isValidDirectoryEntryName(entry.name) || entry.name.empty()
            || entry.name == "." || entry.name == ".."
            || entry.name.find('/') != std::string::npos
            || entry.name.find(char{}) != std::string::npos
            || entry.path.empty()) {
            fail("DirectoryListing converter accepted an invalid entry");
        }
    }
}

void checkConverted(
    const fb::DatasetPageRequestT& wire, const amrvis::DatasetPageRequest& result)
{
    if (!finite(result.region) || !finite(result.slicePosition)
        || result.slicePosition != wire.slice_position) {
        fail("DatasetPageRequest converter accepted a bad request");
    }
}

void checkConverted(
    const fb::DatasetPageResponseT& wire, const amrvis::DatasetPage& result)
{
    if (result.nx < 0 || result.ny < 0
        || result.nx > amrvis::datasetPageMaxExtent
        || result.ny > amrvis::datasetPageMaxExtent) {
        fail("DatasetPageResponse converter accepted a bad extent");
    }
    const auto expected = static_cast<std::size_t>(result.nx)
        * static_cast<std::size_t>(result.ny);
    if (result.values.size() != expected || result.covered.size() != expected
        || !sameBits(result.values, wire.values) || wire.lower.size() != 2
        || wire.upper.size() != 2
        || !std::equal(result.lower.begin(), result.lower.end(),
            wire.lower.begin())) {
        fail("DatasetPageResponse converter accepted inconsistent vectors");
    }
    if (result.hasFiniteValues
        && (!finite(result.minimum) || !finite(result.maximum)
            || result.minimum > result.maximum)) {
        fail("DatasetPageResponse converter accepted a bad value range");
    }
}

void checkConverted(const fb::ParticleSampleRequestT& wire,
    const codec::ParticleSampleRequestData& result)
{
    if (result.species.empty() || result.species != wire.species
        || !(result.fraction > 0.0) || result.fraction > 1.0) {
        fail("ParticleSampleRequest converter accepted a bad request");
    }
}

void checkConverted(
    const fb::ParticleSampleResponseT& wire, const amrvis::ParticleSample& result)
{
    if (result.points.size() != wire.ids.size()
        || wire.positions.size() != 3 * result.points.size()) {
        fail("ParticleSampleResponse converter accepted inconsistent vectors");
    }
    for (std::size_t i = 0; i < result.points.size(); ++i) {
        const auto& point = result.points[i];
        if (point.id != wire.ids[i] || !finite(point.position)
            || !std::equal(point.position.values.begin(),
                point.position.values.end(),
                wire.positions.begin() + static_cast<std::ptrdiff_t>(3 * i))) {
            fail("ParticleSampleResponse converter misread a particle");
        }
    }
}

void checkConverted(const fb::RangeRequestT& wire,
    const std::pair<amrvis::DatasetId, amrvis::RangeRequest>& result)
{
    // The enums went through fromWire; sending them back must reproduce the
    // wire values, or the mapping is not the bijection the server relies on.
    const auto roundTrip = codec::toWire(result.first, result.second);
    if (result.first.value != wire.dataset_id
        || result.second.field.value != wire.field
        || roundTrip.composition != wire.composition
        || roundTrip.scope != wire.scope) {
        fail("RangeRequest did not convert faithfully");
    }
}

void checkConverted(const fb::RangeResponseT& wire,
    const std::optional<amrvis::ValueRange>& result)
{
    if (result.has_value() != wire.has_range) {
        fail("RangeResponse converter lost the has_range flag");
    }
    if (result
        && (!finite(result->minimum) || !finite(result->maximum)
            || result->minimum > result->maximum
            || result->minimum != wire.minimum
            || result->maximum != wire.maximum)) {
        fail("RangeResponse converter accepted a bad range");
    }
}

void checkConverted(
    const fb::ErrorResponseT& wire, const amrvis::remote::ErrorData& result)
{
    if (result.message != wire.message || codec::toWire(result).code != wire.code) {
        fail("ErrorResponse did not convert faithfully");
    }
}

// fromWire on a payload pointer, mirroring the server: a null pointer (the
// union tag disagreeing with the stored table on a crafted buffer) is the
// server's "payload is missing" rejection, not a dereference. An accepted
// payload must satisfy its checkConverted postconditions.
template <typename Payload>
void convert(const Payload* payload)
{
    if (payload != nullptr) {
        checkConverted(*payload, codec::fromWire(*payload));
    }
}

// Route a decoded envelope's payload through the matching fromWire converter
// -- the layer where the server/client actually validate hostile payload
// contents (validateResultVectors, finite-value and enum checks). Every arm
// is named (no default) so a new payload type fails to compile here under
// -Wswitch until it is either converted or listed as trivial; a converter
// throwing std::exception is the expected rejection.
void exerciseFromWire(const codec::NativeEnvelope& envelope)
{
    switch (envelope.payload.type) {
    case fb::Payload::HelloRequest:
        convert(envelope.payload.AsHelloRequest());
        break;
    case fb::Payload::HelloResponse:
        convert(envelope.payload.AsHelloResponse());
        break;
    case fb::Payload::OpenDatasetRequest:
        convert(envelope.payload.AsOpenDatasetRequest());
        break;
    case fb::Payload::DatasetOpened:
        convert(envelope.payload.AsDatasetOpened());
        break;
    case fb::Payload::SliceViewRequest:
        convert(envelope.payload.AsSliceViewRequest());
        break;
    case fb::Payload::SliceViewResponse:
        convert(envelope.payload.AsSliceViewResponse());
        break;
    case fb::Payload::LineViewRequest:
        convert(envelope.payload.AsLineViewRequest());
        break;
    case fb::Payload::LineViewResponse:
        convert(envelope.payload.AsLineViewResponse());
        break;
    case fb::Payload::DatasetPageRequest:
        convert(envelope.payload.AsDatasetPageRequest());
        break;
    case fb::Payload::DatasetPageResponse:
        convert(envelope.payload.AsDatasetPageResponse());
        break;
    case fb::Payload::ParticleSampleRequest:
        convert(envelope.payload.AsParticleSampleRequest());
        break;
    case fb::Payload::ParticleSampleResponse:
        convert(envelope.payload.AsParticleSampleResponse());
        break;
    case fb::Payload::RangeRequest:
        convert(envelope.payload.AsRangeRequest());
        break;
    case fb::Payload::RangeResponse:
        convert(envelope.payload.AsRangeResponse());
        break;
    case fb::Payload::ErrorResponse:
        convert(envelope.payload.AsErrorResponse());
        break;
    case fb::Payload::DirectoryListing:
        convert(envelope.payload.AsDirectoryListing());
        break;
    case fb::Payload::RenderedFrameRequest:
        convert(envelope.payload.AsRenderedFrameRequest());
        break;
    case fb::Payload::RenderedFrameResponse:
        convert(envelope.payload.AsRenderedFrameResponse());
        break;
    case fb::Payload::NONE:
    case fb::Payload::ListDirectoryRequest:
    case fb::Payload::CloseDatasetRequest:
    case fb::Payload::DatasetClosed:
    case fb::Payload::ClearCacheRequest:
    case fb::Payload::SetCacheBudgetRequest:
    case fb::Payload::CacheResponse:
    case fb::Payload::CancelRequest:
    case fb::Payload::CancelAcknowledged:
    case fb::Payload::PingRequest:
    case fb::Payload::PongResponse:
        break;  // trivial payloads: no converter, nothing to validate
    }
}

void exerciseWire(std::span<const std::uint8_t> bytes)
{
    std::unique_ptr<codec::NativeEnvelope> envelope;
    try {
        envelope = codec::decode(bytes);
    } catch (const std::exception&) {
        return;  // the documented rejection, and what production catches
    } catch (...) {
        fail("codec::decode let a non-std::exception escape");
    }
    // Postcondition: a decoded envelope is inspectable -- outside the
    // rejection catch, so an inspect that throws is a failure, not a clean
    // rejection -- and the summary agrees with it, payload kind included: the
    // one field that is a translation, and the one the server dispatches on.
    try {
        const auto info = codec::inspect(*envelope);
        if (info.requestId != envelope->request_id
            || info.protocolMajor != envelope->protocol_major
            || info.protocolMinorVersion != envelope->protocol_minor_version
            || static_cast<std::uint8_t>(info.payload)
                != static_cast<std::uint8_t>(envelope->payload.type)) {
            fail("codec::inspect disagrees with its envelope");
        }
    } catch (const std::exception&) {
        fail("codec::inspect rejected an envelope decode accepted");
    }
    try {
        exerciseFromWire(*envelope);
    } catch (const std::exception&) {
        // Expected rejection path (the codec's documented contract).
    } catch (...) {
        fail("a fromWire converter let a non-std::exception escape");
    }
}

#if !defined(AMREXPLORER_LIBFUZZER)
std::unique_ptr<fb::Real3T> real3(double x, double y, double z)
{
    auto value = std::make_unique<fb::Real3T>();
    value->values = {x, y, z};
    return value;
}

std::unique_ptr<fb::Int3T> int3(int x, int y, int z)
{
    auto value = std::make_unique<fb::Int3T>();
    value->values = {x, y, z};
    return value;
}

std::unique_ptr<fb::RealBoxT> unitBox()
{
    auto box = std::make_unique<fb::RealBoxT>();
    box->lower = real3(0.0, 0.0, 0.0);
    box->upper = real3(1.0, 1.0, 1.0);
    return box;
}

// One seed per fromWire converter arm (plus a trivial ping), each valid enough
// to pass its converter unmutated -- main() checks that -- so mutations reach
// the deep validators (validateResultVectors, finite-value and enum checks)
// with near-valid data instead of dying at a converter's first check.
std::vector<std::vector<std::uint8_t>> wireSeeds()
{
    std::vector<std::vector<std::uint8_t>> seeds;
    std::uint64_t requestId = 0;
    const auto add = [&](auto payload) {
        seeds.push_back(codec::encode(++requestId, std::move(payload)));
    };
    {
        fb::PingRequestT ping;
        ping.nonce = 7;
        add(std::move(ping));
    }
    {
        fb::HelloRequestT hello;
        hello.client_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_minor_version = 1;
        add(std::move(hello));
    }
    {
        fb::HelloResponseT hello;
        hello.server_name = "fuzz";
        hello.maximum_frame_bytes = 1 << 20;
        hello.maximum_datasets = 2;
        hello.capabilities = {1, 2};
        add(std::move(hello));
    }
    {
        fb::OpenDatasetRequestT open;
        open.path = "/plt00000";
        open.cache_budget_bytes = 1 << 20;
        // Protocol 1.4, and non-empty on purpose: an absent vector is the
        // schema default and is left out of the buffer, so a seed without one
        // would be byte-identical to a pre-1.4 request and would reach none of
        // the definition bounds. Two entries so the mutator can corrupt one
        // while the other stays well formed.
        auto first = std::make_unique<fb::DerivedFieldDefinitionT>();
        first->name = "speed";
        first->expression = "sqrt(u**2 + v**2)";
        open.derived_fields.push_back(std::move(first));
        auto second = std::make_unique<fb::DerivedFieldDefinitionT>();
        second->name = "twice";
        second->expression = "2*speed";
        open.derived_fields.push_back(std::move(second));
        add(std::move(open));
    }
    {
        // Two fields on a two-level, two-box catalog: the smallest shape that
        // exercises every count check in fromWire(DatasetOpenedT). Two levels
        // rather than one so finest_level is non-default and therefore
        // present in the buffer for the mutator to reach (FlatBuffers omits
        // default-valued scalars).
        fb::DatasetOpenedT opened;
        opened.dataset_id = 1;
        opened.dimension = 3;
        opened.finest_level = 1;
        opened.time = 0.5;
        opened.physical_domain = unitBox();
        for (const char* name : {"density", "pressure"}) {
            auto field = std::make_unique<fb::FieldCatalogT>();
            field->name = name;
            opened.fields.push_back(std::move(field));
        }
        for (int refinement : {1, 2}) {
            auto level = std::make_unique<fb::LevelCatalogT>();
            level->level = refinement - 1;
            auto domain = std::make_unique<fb::IntBoxT>();
            domain->lower = int3(0, 0, 0);
            domain->upper = int3(4 * refinement - 1, 4 * refinement - 1,
                4 * refinement - 1);
            domain->centering = int3(0, 0, 0);
            level->domain = std::move(domain);
            level->cell_size = real3(
                0.25 / refinement, 0.25 / refinement, 0.25 / refinement);
            level->index_origin = real3(0.0, 0.0, 0.0);
            for (int lo : {0, 2}) {
                auto box = std::make_unique<fb::IntBoxT>();
                box->lower = int3(lo * refinement, 0, 0);
                box->upper = int3(
                    lo * refinement + 1, 4 * refinement - 1, 4 * refinement - 1);
                box->centering = int3(0, 0, 0);
                level->boxes.push_back(std::move(box));
            }
            opened.levels.push_back(std::move(level));
        }
        auto species = std::make_unique<fb::ParticleSpeciesCatalogT>();
        species->name = "electrons";
        species->dimension = 3;
        opened.particle_species.push_back(std::move(species));
        opened.file_range_available = {1, 1};
        opened.level_range_available = {1, 0, 1, 1};
        opened.metadata_metrics = std::make_unique<fb::MetadataReadMetricsT>();
        opened.cache = std::make_unique<fb::CacheStateT>();
        // Protocol 1.4, and non-default on purpose, for the same reason the
        // request seed carries definitions: 0 and an empty vector are the
        // schema defaults and are left out of the buffer, so a seed without
        // them would be byte-identical to a pre-1.4 reply and would reach
        // none of the reply-side bounds. One of the two fields is derived,
        // and one skip so the vector is present for the mutator to grow.
        opened.derived_field_count = 1;
        auto skip = std::make_unique<fb::DerivedFieldSkipT>();
        skip->definition_index = 1;
        skip->name = "twice";
        skip->reason = "no field or coordinate is named 'speed'";
        opened.derived_field_skips.push_back(std::move(skip));
        add(std::move(opened));
    }
    {
        fb::SliceViewRequestT request;
        request.dataset_id = 1;
        request.field = 0;
        request.visible_region = unitBox();
        request.width = 4;
        request.height = 4;
        add(std::move(request));
    }
    {
        fb::SliceViewResponseT slice;
        slice.width = 2;
        slice.height = 2;
        slice.physical_region = unitBox();
        slice.values = {1.0F, 2.0F, 3.0F, 4.0F};
        slice.valid = {1, 1, 1, 1};
        slice.source_level = {0, 0, 0, 0};
        auto gridBox = std::make_unique<fb::SliceGridBoxT>();
        gridBox->physical_region = unitBox();
        slice.grid_boxes.push_back(std::move(gridBox));
        slice.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(slice));
    }
    {
        fb::LineViewRequestT request;
        request.dataset_id = 1;
        request.axis = 0;
        request.fixed_coordinates = real3(0.5, 0.5, 0.5);
        request.region = unitBox();
        request.has_region = true;
        request.output_width = 8;
        add(std::move(request));
    }
    {
        fb::LineViewResponseT line;
        line.positions = {0.5, 1.5};
        line.values = {1.0, 2.0};
        line.valid = {1, 1};
        line.source_level = {0, 0};
        line.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(line));
    }
    {
        fb::DatasetPageRequestT request;
        request.dataset_id = 1;
        request.region = unitBox();
        request.slice_position = 0.5;
        request.maximum_extent = 8;
        add(std::move(request));
    }
    {
        fb::DatasetPageResponseT page;
        page.lower = {0, 0};
        page.upper = {1, 1};
        page.nx = 2;
        page.ny = 2;
        page.values = {1.0F, 2.0F, 3.0F, 4.0F};
        page.covered = {1, 1, 1, 1};
        page.minimum = 1.0;
        page.maximum = 4.0;
        page.has_finite_values = true;
        page.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(page));
    }
    {
        fb::ParticleSampleRequestT request;
        request.dataset_id = 1;
        request.species = "electrons";
        request.fraction = 0.5;
        request.seed = 3;
        add(std::move(request));
    }
    {
        fb::ParticleSampleResponseT particles;
        particles.species = std::make_unique<fb::ParticleSpeciesCatalogT>();
        particles.species->name = "electrons";
        particles.species->dimension = 3;
        particles.ids = {11, 12};
        particles.positions = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
        particles.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(particles));
    }
    {
        fb::RangeRequestT request;
        request.dataset_id = 1;
        request.field = 0;
        request.scope = fb::RangeScope::Level;
        add(std::move(request));
    }
    {
        fb::RangeResponseT range;
        range.has_range = true;
        range.minimum = -1.0;
        range.maximum = 1.0;
        range.cache = std::make_unique<fb::CacheStateT>();
        add(std::move(range));
    }
    {
        fb::ErrorResponseT error;
        error.code = fb::ErrorCode::InvalidRequest;
        error.message = "boom";
        add(std::move(error));
    }
    {
        fb::DirectoryListingT listing;
        listing.path = "/scratch/run";
        listing.parent_path = "/scratch";
        // Non-default, so the flag has a byte in the buffer for the
        // corruption sweep to reach.
        listing.truncated = true;
        auto plotfile = std::make_unique<fb::DirectoryEntryT>();
        plotfile->name = "plt00010";
        plotfile->path = "/scratch/run/plt00010";
        plotfile->is_plotfile = true;
        listing.entries.push_back(std::move(plotfile));
        auto directory = std::make_unique<fb::DirectoryEntryT>();
        directory->name = "inputs";
        directory->path = "/scratch/run/inputs";
        listing.entries.push_back(std::move(directory));
        add(std::move(listing));
    }
    {
        // Protocol 1.2: a volume request with an explicit range and a
        // four-entry transfer function, and a 2x2 frame.
        amrvis::VolumeRenderRequest request;
        request.dataset.value = 3;
        request.field.value = 1;
        request.maximumLevel = 1;
        request.composition = amrvis::CompositionPolicy::ExactLevel;
        request.region.lower = {{0.0, 0.0, 0.0}};
        request.region.upper = {{1.0, 2.0, 3.0}};
        request.camera = {0.5, -0.25, 1.5};
        request.outputSize = {64, 48};
        request.range = amrvis::VolumeRange{0.5, 2.0, true};
        // Not the *wire* default: Nearest is the schema's zero, which
        // flatbuffers omits from the buffer entirely, so a seed carrying it
        // would be byte-identical to a pre-1.3 request and would exercise
        // nothing. This one has to be written and read back.
        request.sampling = amrvis::SamplingPolicy::Linear;
        request.transfer.colors = {0x0000FFU, 0x00FF00U, 0xFFFF00U, 0xFF0000U};
        request.transfer.opacities = {0.0F, 0.25F, 0.5F, 1.0F};
        request.samplesPerVoxel = 3;
        request.maximumVoxels = 4096;
        add(codec::toWire(request));
        amrvis::VolumeFrame frame;
        frame.width = 2;
        frame.height = 2;
        frame.pixels = {0xFF102030U, 0x80402010U, 0U, 0xFFFFFFFFU};
        frame.usedRange = {0.5, 2.0, true};
        frame.metrics.gridDims = {4, 4, 4};
        frame.metrics.coveredVoxels = 60;
        frame.metrics.sampledMaximumLevel = 1;
        frame.metrics.gridFromCache = true;
        frame.metrics.blocksRead = 2;
        frame.cacheFallbackFromLevel = 1;
        frame.cacheFallbackToLevel = 0;
        add(codec::toWire(frame, amrvis::CacheMetrics{}));
    }
    return seeds;
}
#endif

} // namespace

#if defined(AMREXPLORER_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    amrvis::fuzz::setCurrentInput(0, data, size);
    exerciseWire({data, size});
    return 0;
}
#else
int main()
{
    constexpr int iterations = 60000;
    std::uint64_t rng = 0x1234'5678'9abc'def0ULL;
    const auto seeds = wireSeeds();
    long iteration = 0;
    // A seed its own converter rejects would only ever exercise that first
    // check, so require every seed to decode and convert unmutated.
    for (const auto& seed : seeds) {
        amrvis::fuzz::setCurrentInput(iteration++, seed.data(), seed.size());
        try {
            exerciseFromWire(*codec::decode(seed));
        } catch (const std::exception&) {
            fail("a wire seed is rejected before mutation");
        }
    }
    // Systematic single-byte corruption of every seed: each byte set to each
    // of a few extreme values, so every field of every payload -- counts,
    // enum tags, offsets, the top byte of every double -- is perturbed
    // deterministically rather than when the random stream happens to land
    // on it (a ~800-byte seed sees a given byte only a few times in 60000
    // random edits).
    for (const auto& seed : seeds) {
        for (std::size_t offset = 0; offset < seed.size(); ++offset) {
            for (const std::uint8_t value : std::array<std::uint8_t, 5>{
                     0x00, 0x01, 0x7F, 0x80, 0xFF}) {
                auto corrupted = seed;
                corrupted[offset] = value;
                amrvis::fuzz::setCurrentInput(
                    iteration++, corrupted.data(), corrupted.size());
                exerciseWire(corrupted);
            }
        }
    }
    for (int i = 0; i < iterations; ++i) {
        // Purely random bytes can never carry the AVR2 file identifier, so
        // decode would reject every one at its first check and the verifier
        // would sit idle. Stamp the identifier at its flatbuffer offset
        // (bytes 4..8) on half the buffers so random inputs reach the
        // Verifier -- its rejection paths on garbage are the target here; a
        // random root offset and vtable never pass it, so the mutated seeds
        // below are what reach the envelope checks and the converters.
        auto bytes = amrvis::fuzz::randomBytes(rng, 512);
        if (bytes.size() >= 8 && (i % 2) == 0) {
            std::memcpy(bytes.data() + 4, "AVR2", 4);
        }
        amrvis::fuzz::setCurrentInput(iteration++, bytes.data(), bytes.size());
        exerciseWire(bytes);
        const auto mutated = amrvis::fuzz::mutate(
            rng, seeds[amrvis::fuzz::nextRandom(rng) % seeds.size()]);
        amrvis::fuzz::setCurrentInput(
            iteration++, mutated.data(), mutated.size());
        exerciseWire(mutated);
    }
    std::printf("fuzz_wire_codec: %d iterations, no crash\n", iterations);
    return 0;
}
#endif
