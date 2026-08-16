#pragma once

#include "amrexplorer_wire_generated.h"

#include <amrexplorer/remote/Protocol.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace amrvis::remote::codec {

namespace fb = amrexplorer::wire;
using Bytes = std::vector<std::uint8_t>;
using NativeEnvelope = fb::EnvelopeT;

template <typename Payload>
Bytes encode(std::uint64_t requestId, Payload payload,
    std::uint16_t minorVersion = protocolMinorVersion)
{
    if (requestId == 0) {
        throw std::invalid_argument("wire request ID must be non-zero");
    }
    NativeEnvelope envelope;
    envelope.protocol_major = protocolMajor;
    envelope.protocol_minor_version = minorVersion;
    envelope.request_id = requestId;
    envelope.payload.Set(std::move(payload));
    flatbuffers::FlatBufferBuilder builder;
    const auto packed = fb::Envelope::Pack(builder, &envelope);
    fb::FinishEnvelopeBuffer(builder, packed);
    return {builder.GetBufferPointer(),
        builder.GetBufferPointer() + builder.GetSize()};
}

[[nodiscard]] std::unique_ptr<NativeEnvelope> decode(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] EnvelopeInfo inspect(const NativeEnvelope& envelope);

[[nodiscard]] std::unique_ptr<fb::Real3T> toWire(const Real3& value);
[[nodiscard]] std::unique_ptr<fb::Int3T> toWire(const Int3& value);
[[nodiscard]] std::unique_ptr<fb::RealBoxT> toWire(const RealBox& value);
[[nodiscard]] std::unique_ptr<fb::IntBoxT> toWire(const IntBox& value);
[[nodiscard]] Real3 fromWire(const fb::Real3T* value);
[[nodiscard]] Int3 fromWire(const fb::Int3T* value);
[[nodiscard]] RealBox fromWire(const fb::RealBoxT* value);
[[nodiscard]] IntBox fromWire(const fb::IntBoxT* value);

[[nodiscard]] std::unique_ptr<fb::CacheStateT> toWire(
    const CacheMetrics& value);
[[nodiscard]] CacheMetrics fromWire(const fb::CacheStateT* value);

[[nodiscard]] fb::HelloRequestT toWire(const HelloRequestData& value);
[[nodiscard]] HelloRequestData fromWire(const fb::HelloRequestT& value);
[[nodiscard]] fb::HelloResponseT toWire(const HelloResponseData& value);
[[nodiscard]] HelloResponseData fromWire(const fb::HelloResponseT& value);
[[nodiscard]] fb::OpenDatasetRequestT toWire(const OpenDatasetData& value);
[[nodiscard]] OpenDatasetData fromWire(
    const fb::OpenDatasetRequestT& value);
// A directory entry name a listing may carry: one path component, so
// non-empty, not "." or "..", and without '/'. A backslash is a legal
// filename character on the Linux servers the client browses, so it passes.
// Shared with the fuzz harness, which mirrors the check.
[[nodiscard]] bool isValidDirectoryEntryName(std::string_view name) noexcept;
[[nodiscard]] fb::ListDirectoryRequestT toWireDirectoryRequest(
    const std::string& path);
[[nodiscard]] fb::DirectoryListingT toWire(
    const RemoteDirectoryListing& value);
[[nodiscard]] RemoteDirectoryListing fromWire(
    const fb::DirectoryListingT& value);
[[nodiscard]] fb::DatasetOpenedT toWire(const OpenedDataset& value);
[[nodiscard]] OpenedDataset fromWire(const fb::DatasetOpenedT& value);

[[nodiscard]] fb::SliceViewRequestT toWire(const SliceRequest& value);
[[nodiscard]] SliceRequest fromWire(const fb::SliceViewRequestT& value);
[[nodiscard]] fb::SliceViewResponseT toWire(
    const SliceQueryResult& value, const CacheMetrics& cache);
[[nodiscard]] SliceQueryResult fromWire(
    const fb::SliceViewResponseT& value);

[[nodiscard]] fb::LineViewRequestT toWire(const LineViewRequest& value);
[[nodiscard]] LineViewRequest fromWire(const fb::LineViewRequestT& value);
[[nodiscard]] fb::LineViewResponseT toWire(
    const LineQueryResult& value, const CacheMetrics& cache);
[[nodiscard]] LineQueryResult fromWire(
    const fb::LineViewResponseT& value);

[[nodiscard]] fb::DatasetPageRequestT toWire(
    const DatasetPageRequest& value);
[[nodiscard]] DatasetPageRequest fromWire(
    const fb::DatasetPageRequestT& value);
[[nodiscard]] fb::DatasetPageResponseT toWire(
    const DatasetPage& value, const CacheMetrics& cache);
[[nodiscard]] DatasetPage fromWire(const fb::DatasetPageResponseT& value);

[[nodiscard]] fb::ParticleSampleRequestT toWire(DatasetId dataset,
    const std::string& species, double fraction, std::uint64_t seed);
struct ParticleSampleRequestData {
    DatasetId dataset;
    std::string species;
    double fraction = 0.0;
    std::uint64_t seed = 0;
};
[[nodiscard]] ParticleSampleRequestData fromWire(
    const fb::ParticleSampleRequestT& value);
[[nodiscard]] fb::ParticleSampleResponseT toWire(
    const ParticleSample& value, const CacheMetrics& cache);
[[nodiscard]] ParticleSample fromWire(
    const fb::ParticleSampleResponseT& value);

[[nodiscard]] fb::RangeRequestT toWire(
    DatasetId dataset, const RangeRequest& value);
[[nodiscard]] std::pair<DatasetId, RangeRequest> fromWire(
    const fb::RangeRequestT& value);
[[nodiscard]] fb::RangeResponseT toWire(
    const std::optional<ValueRange>& value, const CacheMetrics& cache);
[[nodiscard]] std::optional<ValueRange> fromWire(
    const fb::RangeResponseT& value);

[[nodiscard]] fb::ErrorResponseT toWire(const ErrorData& value);
[[nodiscard]] ErrorData fromWire(const fb::ErrorResponseT& value);

} // namespace amrvis::remote::codec
