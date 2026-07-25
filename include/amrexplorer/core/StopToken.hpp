#pragma once

#include <version>

#if !defined(AMREXPLORER_TEST_FORCE_FALLBACK_STOP_TOKEN) \
    && defined(__cpp_lib_jthread) && __cpp_lib_jthread >= 201911L

#include <stop_token>

namespace amrvis {

using StopSource = std::stop_source;
using StopToken = std::stop_token;

} // namespace amrvis

#else

#include <atomic>
#include <memory>
#include <utility>

namespace amrvis {

class StopSource;

// Minimal C++20 stop-token fallback for standard libraries that do not yet
// implement P0660. Amrvis only polls for cancellation, so callbacks are not
// part of this compatibility type.
class StopToken {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return m_state != nullptr
            && m_state->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept
    {
        return m_state != nullptr;
    }

private:
    friend class StopSource;

    explicit StopToken(std::shared_ptr<std::atomic_bool> state) noexcept
        : m_state(std::move(state))
    {
    }

    std::shared_ptr<std::atomic_bool> m_state;
};

class StopSource {
public:
    StopSource()
        : m_state(std::make_shared<std::atomic_bool>(false))
    {
    }

    [[nodiscard]] StopToken get_token() const noexcept
    {
        return StopToken(m_state);
    }

    bool request_stop() const noexcept
    {
        return m_state != nullptr
            && !m_state->exchange(true, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        return m_state != nullptr
            && m_state->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept
    {
        return m_state != nullptr;
    }

private:
    std::shared_ptr<std::atomic_bool> m_state;
};

} // namespace amrvis

#endif
