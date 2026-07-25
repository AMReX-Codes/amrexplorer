#include "Frame.hpp"
#include "amrvis_wire_generated.h"

#include <amrexplorer/io/PlotfileDataset.hpp>
#include <amrexplorer/query/SliceQuery.hpp>

#include <flatbuffers/flatbuffers.h>
#include <flatbuffers/verifier.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fb = amrvis::wire;
namespace transport = amrvis::wire::prototype;

[[nodiscard]] std::uint16_t parsePort(const char* text) {
    const auto value = std::stoul(text);
    if (value > 65535U) {
        throw std::invalid_argument("port must be in the range 0..65535");
    }
    return static_cast<std::uint16_t>(value);
}

void sendBuilder(const transport::Socket& socket, const flatbuffers::FlatBufferBuilder& builder) {
    transport::writeFrame(
        socket, std::span<const std::uint8_t>(builder.GetBufferPointer(), builder.GetSize()));
}

void sendError(const transport::Socket& socket, std::uint64_t requestId, std::uint32_t code,
               const std::string& message) {
    flatbuffers::FlatBufferBuilder builder;
    const auto text = builder.CreateString(message);
    const auto error = fb::CreateErrorResponse(builder, code, text);
    const auto envelope =
        fb::CreateEnvelope(builder, transport::protocolMajor, transport::protocolMinor, requestId,
                           fb::Payload_ErrorResponse, error.Union());
    fb::FinishEnvelopeBuffer(builder, envelope);
    sendBuilder(socket, builder);
}

[[nodiscard]] bool verifyFrame(const std::vector<std::uint8_t>& frame) {
    if (frame.size() < 8 || !fb::EnvelopeBufferHasIdentifier(frame.data())) {
        return false;
    }
    flatbuffers::Verifier verifier(frame.data(), frame.size());
    return fb::VerifyEnvelopeBuffer(verifier);
}

[[nodiscard]] amrvis::SamplingPolicy samplingPolicy(fb::SamplingPolicy policy) {
    switch (policy) {
    case fb::SamplingPolicy_Nearest:
        return amrvis::SamplingPolicy::Nearest;
    case fb::SamplingPolicy_PiecewiseConstant:
        return amrvis::SamplingPolicy::PiecewiseConstant;
    case fb::SamplingPolicy_Linear:
        return amrvis::SamplingPolicy::Linear;
    }
    throw std::invalid_argument("unknown sampling policy");
}

[[nodiscard]] amrvis::CompositionPolicy compositionPolicy(fb::CompositionPolicy policy) {
    switch (policy) {
    case fb::CompositionPolicy_FinestAvailable:
        return amrvis::CompositionPolicy::FinestAvailable;
    case fb::CompositionPolicy_ExactLevel:
        return amrvis::CompositionPolicy::ExactLevel;
    }
    throw std::invalid_argument("unknown composition policy");
}

[[nodiscard]] amrvis::RealBox realBox(const flatbuffers::Vector<double>* lower,
                                      const flatbuffers::Vector<double>* upper) {
    if (lower == nullptr || upper == nullptr || lower->size() != 3 || upper->size() != 3) {
        throw std::invalid_argument("physical box vectors must contain exactly three values");
    }
    amrvis::RealBox box;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        box.lower[axis] = lower->Get(static_cast<flatbuffers::uoffset_t>(axis));
        box.upper[axis] = upper->Get(static_cast<flatbuffers::uoffset_t>(axis));
    }
    return box;
}

class Session {
  public:
    explicit Session(const transport::Socket& socket) : m_socket(socket) {}

    [[nodiscard]] bool handle(const fb::Envelope& envelope) {
        if (envelope.protocol_major() != transport::protocolMajor) {
            sendError(m_socket, envelope.request_id(), 1, "unsupported protocol major version");
            return false;
        }
        switch (envelope.payload_type()) {
        case fb::Payload_HelloRequest:
            hello(envelope);
            return false;
        case fb::Payload_OpenDatasetRequest:
            openDataset(envelope);
            return false;
        case fb::Payload_SliceRequest:
            querySlice(envelope);
            return true;
        default:
            sendError(m_socket, envelope.request_id(), 2,
                      "payload is not a supported client request");
            return false;
        }
    }

  private:
    void hello(const fb::Envelope& envelope) const {
        flatbuffers::FlatBufferBuilder builder;
        const auto name = builder.CreateString("amrvis_wire_server prototype");
        const auto response = fb::CreateHelloResponse(builder, name);
        const auto reply =
            fb::CreateEnvelope(builder, transport::protocolMajor, transport::protocolMinor,
                               envelope.request_id(), fb::Payload_HelloResponse, response.Union());
        fb::FinishEnvelopeBuffer(builder, reply);
        sendBuilder(m_socket, builder);
    }

    void openDataset(const fb::Envelope& envelope) {
        const auto* request = envelope.payload_as_OpenDatasetRequest();
        if (request == nullptr || request->path() == nullptr || request->path()->empty()) {
            throw std::invalid_argument("dataset path is empty");
        }
        m_dataset = std::make_unique<amrvis::PlotfileDataset>(
            std::filesystem::path(request->path()->str()), amrvis::DatasetId{m_nextDatasetId++},
            request->cache_budget_bytes());
        const auto& metadata = m_dataset->metadata();

        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<fb::FieldMetadata>> fieldOffsets;
        fieldOffsets.reserve(metadata.fields.size());
        for (const auto& field : metadata.fields) {
            fieldOffsets.push_back(fb::CreateFieldMetadata(
                builder, builder.CreateString(field.name), field.componentCount));
        }
        const auto lower = builder.CreateVector(metadata.physicalDomain.lower.values.data(),
                                                metadata.physicalDomain.lower.values.size());
        const auto upper = builder.CreateVector(metadata.physicalDomain.upper.values.data(),
                                                metadata.physicalDomain.upper.values.size());
        const auto fields = builder.CreateVector(fieldOffsets);
        const auto opened =
            fb::CreateDatasetOpened(builder, m_dataset->id().value, metadata.dimension,
                                    metadata.finestLevel, metadata.time, lower, upper, fields);
        const auto reply =
            fb::CreateEnvelope(builder, transport::protocolMajor, transport::protocolMinor,
                               envelope.request_id(), fb::Payload_DatasetOpened, opened.Union());
        fb::FinishEnvelopeBuffer(builder, reply);
        sendBuilder(m_socket, builder);
    }

    void querySlice(const fb::Envelope& envelope) {
        if (m_dataset == nullptr) {
            throw std::logic_error("open a dataset before requesting a slice");
        }
        const auto* request = envelope.payload_as_SliceRequest();
        if (request == nullptr) {
            throw std::invalid_argument("slice payload is missing");
        }
        amrvis::SliceRequest query;
        query.dataset = amrvis::DatasetId{request->dataset_id()};
        query.field = amrvis::FieldId{request->field()};
        query.normalDirection = request->normal_direction();
        query.physicalPosition = request->physical_position();
        query.visibleRegion = realBox(request->visible_lower(), request->visible_upper());
        query.maximumLevel = request->maximum_level();
        query.outputSize = {{request->width(), request->height()}};
        query.sampling = samplingPolicy(request->sampling());
        query.composition = compositionPolicy(request->composition());

        const auto result = amrvis::SliceQuery(*m_dataset).execute(query);
        flatbuffers::FlatBufferBuilder builder;
        const auto lower = builder.CreateVector(result.plane.physicalRegion.lower.values.data(),
                                                result.plane.physicalRegion.lower.values.size());
        const auto upper = builder.CreateVector(result.plane.physicalRegion.upper.values.data(),
                                                result.plane.physicalRegion.upper.values.size());
        const auto values = builder.CreateVector(result.plane.values);
        const auto valid = builder.CreateVector(result.plane.valid);
        const auto sourceLevel = builder.CreateVector(result.plane.sourceLevel);
        const auto plane = fb::CreateScalarPlane(
            builder, result.plane.width, result.plane.height, lower, upper, values, valid,
            sourceLevel, result.metrics.candidateBlocks, result.metrics.blocksRead,
            result.metrics.cacheHits, result.metrics.payloadBytesRead);
        const auto reply =
            fb::CreateEnvelope(builder, transport::protocolMajor, transport::protocolMinor,
                               envelope.request_id(), fb::Payload_ScalarPlane, plane.Union());
        fb::FinishEnvelopeBuffer(builder, reply);
        sendBuilder(m_socket, builder);
    }

    const transport::Socket& m_socket;
    std::unique_ptr<amrvis::PlotfileDataset> m_dataset;
    std::uint64_t m_nextDatasetId = 1;
};

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        std::uint16_t port = 0;
        if (argc == 3 && std::string(argv[1]) == "--port") {
            port = parsePort(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("usage: amrvis_wire_server [--port PORT]");
        }

        auto listener = transport::listenOnLoopback(port);
        std::cout << "LISTENING 127.0.0.1 " << listener.port << '\n' << std::flush;
        auto connection = transport::acceptConnection(listener.socket);
        Session session(connection);
        bool complete = false;
        while (!complete) {
            const auto frame = transport::readFrame(connection);
            if (!verifyFrame(frame)) {
                throw std::runtime_error("received an invalid AVR2 FlatBuffer");
            }
            const auto* envelope = fb::GetEnvelope(frame.data());
            try {
                complete = session.handle(*envelope);
            } catch (const std::exception& error) {
                sendError(connection, envelope->request_id(), 3, error.what());
            }
        }
        std::cout << "COMPLETE\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server error: " << error.what() << '\n';
        return 1;
    }
}
