#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace amrvis {

// Admission control for cached payloads across otherwise independent LRUs.
// Transient query/render allocations are deliberately outside this allowance.
class SharedCacheBudget {
public:
    struct Participant {
        virtual ~Participant() = default;
        // Must not wait for another cache's lock: callers can hold their own.
        virtual void reclaimUnpinned() = 0;
    };

    explicit SharedCacheBudget(std::uint64_t bytes)
        : m_limit(bytes)
    {
    }
    [[nodiscard]] std::uint64_t limit() const noexcept { return m_limit; }
    [[nodiscard]] std::uint64_t used() const
    {
        std::scoped_lock lock(m_mutex);
        return m_used;
    }
    void add(const std::shared_ptr<Participant>& participant)
    {
        std::scoped_lock lock(m_mutex);
        std::erase_if(m_participants, [](const auto& p) { return p.expired(); });
        m_participants.push_back(participant);
    }
    [[nodiscard]] bool tryReserve(std::uint64_t bytes)
    {
        std::scoped_lock lock(m_mutex);
        if (bytes > m_limit - m_used) {
            return false;
        }
        m_used += bytes;
        return true;
    }
    [[nodiscard]] bool reserveWithReclaim(std::uint64_t bytes, const Participant* caller)
    {
        if (tryReserve(bytes)) {
            return true;
        }
        if (bytes > m_limit) {
            return false;
        }
        std::vector<std::weak_ptr<Participant>> participants;
        {
            std::scoped_lock lock(m_mutex);
            participants = m_participants;
        }
        for (const auto& weak : participants) {
            if (const auto participant = weak.lock(); participant && participant.get() != caller) {
                participant->reclaimUnpinned();
                if (tryReserve(bytes)) {
                    return true;
                }
            }
        }
        return false;
    }
    void release(std::uint64_t bytes) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_used -= bytes;
    }

private:
    const std::uint64_t m_limit;
    mutable std::mutex m_mutex;
    std::uint64_t m_used = 0;
    std::vector<std::weak_ptr<Participant>> m_participants;
};

} // namespace amrvis
