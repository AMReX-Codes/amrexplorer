#pragma once

#include <amrexplorer/remote/Protocol.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace amrvis::remote::debug {

inline bool traceEnabled() noexcept
{
    static const bool enabled = [] {
#ifdef _WIN32
        char* value = nullptr;
        std::size_t valueLength = 0;
        if (_dupenv_s(&value, &valueLength, "AMREXPLORER_REMOTE_TRACE") != 0
            || value == nullptr) {
            return false;
        }
        if (*value == '\0') {
            std::free(value);
            return false;
        }
        const std::string_view setting(value);
        const bool isEnabled = setting != "0" && setting != "false"
            && setting != "FALSE" && setting != "off"
            && setting != "OFF";
        std::free(value);
        return isEnabled;
#else
        const char* value = std::getenv("AMREXPLORER_REMOTE_TRACE");
        if (value == nullptr || *value == '\0') {
            return false;
        }
        const std::string_view setting(value);
        return setting != "0" && setting != "false"
            && setting != "FALSE" && setting != "off"
            && setting != "OFF";
#endif
    }();
    return enabled;
}

inline std::string_view payloadName(PayloadKind kind) noexcept
{
    switch (kind) {
    case PayloadKind::None: return "none";
    case PayloadKind::HelloRequest: return "hello-request";
    case PayloadKind::HelloResponse: return "hello-response";
    case PayloadKind::OpenDatasetRequest: return "open-dataset";
    case PayloadKind::DatasetOpened: return "dataset-opened";
    case PayloadKind::CloseDatasetRequest: return "close-dataset";
    case PayloadKind::DatasetClosed: return "dataset-closed";
    case PayloadKind::SliceViewRequest: return "slice-request";
    case PayloadKind::SliceViewResponse: return "slice-response";
    case PayloadKind::LineViewRequest: return "line-request";
    case PayloadKind::LineViewResponse: return "line-response";
    case PayloadKind::DatasetPageRequest: return "dataset-page-request";
    case PayloadKind::DatasetPageResponse: return "dataset-page-response";
    case PayloadKind::ParticleSampleRequest: return "particle-request";
    case PayloadKind::ParticleSampleResponse: return "particle-response";
    case PayloadKind::RangeRequest: return "range-request";
    case PayloadKind::RangeResponse: return "range-response";
    case PayloadKind::ClearCacheRequest: return "clear-cache";
    case PayloadKind::SetCacheBudgetRequest: return "set-cache-budget";
    case PayloadKind::CacheResponse: return "cache-response";
    case PayloadKind::CancelRequest: return "cancel-request";
    case PayloadKind::CancelAcknowledged: return "cancel-acknowledged";
    case PayloadKind::PingRequest: return "ping-request";
    case PayloadKind::PongResponse: return "pong-response";
    case PayloadKind::ErrorResponse: return "error-response";
    }
    return "unknown";
}

inline std::mutex& traceMutex() noexcept
{
    static std::mutex mutex;
    return mutex;
}

inline const std::chrono::steady_clock::time_point& traceOrigin() noexcept
{
    static const auto origin = std::chrono::steady_clock::now();
    return origin;
}

template <typename... Values>
void trace(std::string_view side, Values&&... values) noexcept
{
    if (!traceEnabled()) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - traceOrigin());
    try {
        std::scoped_lock lock(traceMutex());
        std::cerr << "[AMREXPLORER-REMOTE] +" << elapsed.count()
                  << "ms " << side << " thread="
                  << std::this_thread::get_id() << ' ';
        (std::cerr << ... << std::forward<Values>(values));
        std::cerr << '\n';
    } catch (...) {
    }
}

} // namespace amrvis::remote::debug
