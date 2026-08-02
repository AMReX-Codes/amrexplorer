#include <amrexplorer/data/SessionValidation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace amrvis {
namespace {

void requireFieldAndLevel(const DatasetMetadata& metadata, FieldId field,
    int maximumLevel, const char* operation)
{
    if (field.value >= metadata.fields.size()) {
        throw std::invalid_argument(
            std::string(operation) + " field is unavailable");
    }
    if (maximumLevel < 0 || maximumLevel > metadata.finestLevel) {
        throw std::invalid_argument(
            std::string(operation) + " level is unavailable");
    }
}

void requireComponent(const DatasetMetadata& metadata, FieldId field,
    int component, const char* operation)
{
    if (component < 0) {
        throw std::invalid_argument(
            std::string(operation) + " component is unavailable");
    }
    const auto& components
        = metadata.fields[static_cast<std::size_t>(field.value)]
              .componentNames;
    const auto count = std::max<std::size_t>(1, components.size());
    if (static_cast<std::size_t>(component) >= count) {
        throw std::invalid_argument(
            std::string(operation) + " component is unavailable");
    }
}

} // namespace

void validateSessionViewRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const ViewDataRequest& request)
{
    std::visit(
        [&](const auto& typed) {
            using Request = std::decay_t<decltype(typed)>;
            const auto& query = [&]() -> const auto& {
                if constexpr (std::is_same_v<Request, SliceRequest>) {
                    return typed;
                } else {
                    return typed.query;
                }
            }();
            if (query.dataset != dataset) {
                throw std::invalid_argument(
                    "view request uses the wrong dataset");
            }
            requireFieldAndLevel(
                metadata, query.field, query.maximumLevel, "view");
            requireComponent(
                metadata, query.field, query.component, "view");
            if constexpr (std::is_same_v<Request, SliceRequest>) {
                const auto errors
                    = validateSliceRequest(typed, metadata.dimension);
                if (!errors.empty()) {
                    throw std::invalid_argument(errors.front());
                }
            } else {
                const auto errors
                    = validateLineRequest(typed.query, metadata.dimension);
                if (!errors.empty()) {
                    throw std::invalid_argument(errors.front());
                }
                if (typed.outputWidth < 1) {
                    throw std::invalid_argument(
                        "line output width must be positive");
                }
            }
        },
        request);
}

void validateSessionDatasetPageRequest(const DatasetMetadata& metadata,
    DatasetId dataset, const DatasetPageRequest& request)
{
    if (request.dataset != dataset) {
        throw std::invalid_argument("dataset page uses the wrong dataset");
    }
    if (metadata.dimension < 2 || metadata.dimension > 3) {
        throw std::invalid_argument(
            "dataset page requires a 2-D or 3-D dataset");
    }
    if (request.level < 0
        || request.level >= static_cast<int>(metadata.levels.size())) {
        throw std::invalid_argument("dataset page level is unavailable");
    }
    if (request.field.value >= metadata.fields.size()) {
        throw std::invalid_argument("dataset page field is unavailable");
    }
    if (metadata.dimension == 3
        && (request.normalAxis < 0 || request.normalAxis > 2)) {
        throw std::invalid_argument("dataset page normal axis is invalid");
    }
    if (request.maximumExtent < 1
        || request.maximumExtent > datasetPageMaxExtent) {
        throw std::invalid_argument(
            "dataset page extent is outside its limit");
    }
    if (!std::isfinite(request.slicePosition)) {
        throw std::invalid_argument(
            "dataset page slice position must be finite");
    }
}

void validateSessionRangeRequest(
    const DatasetMetadata& metadata, const RangeRequest& request)
{
    requireFieldAndLevel(
        metadata, request.field, request.maximumLevel, "range");
    if (request.composition != CompositionPolicy::FinestAvailable
        && request.composition != CompositionPolicy::ExactLevel) {
        throw std::invalid_argument("range composition policy is invalid");
    }
    if (request.scope != RangeScope::File
        && request.scope != RangeScope::Level) {
        throw std::invalid_argument("range scope is invalid");
    }
}

void validateSessionParticleRequest(const DatasetMetadata& metadata,
    const std::vector<ParticleSpeciesMetadata>& species,
    const std::string& name, double fraction)
{
    static_cast<void>(metadata);
    if (name.empty()
        || std::none_of(species.begin(), species.end(),
            [&](const auto& entry) { return entry.name == name; })) {
        throw std::invalid_argument("particle species is unavailable");
    }
    if (!std::isfinite(fraction) || !(fraction > 0.0) || fraction > 1.0) {
        throw std::invalid_argument(
            "particle sample fraction must be in (0, 1]");
    }
}

} // namespace amrvis
