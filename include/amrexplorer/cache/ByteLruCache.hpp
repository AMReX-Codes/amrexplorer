#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace amrvis {

class CacheBudgetExceeded : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

template <typename Key, typename Value, typename Hash = std::hash<Key>>
class ByteLruCache {
private:
    struct Entry;
    struct State;

public:
    class Handle {
    public:
        Handle() = default;
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;

        Handle(Handle&& other) noexcept
            : m_state(std::move(other.m_state))
            , m_key(std::move(other.m_key))
            , m_value(std::move(other.m_value))
            , m_bytes(std::exchange(other.m_bytes, 0))
            , m_pinned(std::exchange(other.m_pinned, false))
        {
        }

        Handle& operator=(Handle&& other) noexcept
        {
            if (this != &other) {
                release();
                m_state = std::move(other.m_state);
                m_key = std::move(other.m_key);
                m_value = std::move(other.m_value);
                m_bytes = std::exchange(other.m_bytes, 0);
                m_pinned = std::exchange(other.m_pinned, false);
            }
            return *this;
        }

        ~Handle() { release(); }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(m_value);
        }

        [[nodiscard]] const Value& operator*() const { return *m_value; }
        [[nodiscard]] const Value* operator->() const noexcept { return m_value.get(); }
        [[nodiscard]] const std::shared_ptr<const Value>& value() const noexcept
        {
            return m_value;
        }
        [[nodiscard]] std::uint64_t bytes() const noexcept { return m_bytes; }

    private:
        friend class ByteLruCache;

        Handle(std::shared_ptr<State> state, Key key,
            std::shared_ptr<const Value> value, std::uint64_t bytes)
            : m_state(std::move(state))
            , m_key(std::move(key))
            , m_value(std::move(value))
            , m_bytes(bytes)
            , m_pinned(true)
        {
        }

        void release() noexcept
        {
            if (!m_pinned || !m_state) {
                return;
            }
            std::scoped_lock lock(m_state->mutex);
            const auto found = m_state->entries.find(m_key);
            if (found != m_state->entries.end() && found->second.pinCount > 0) {
                --found->second.pinCount;
                if (found->second.pinCount == 0) {
                    m_state->metrics.pinnedBytes -= found->second.bytes;
                }
            }
            m_pinned = false;
        }

        std::shared_ptr<State> m_state;
        Key m_key{};
        std::shared_ptr<const Value> m_value;
        std::uint64_t m_bytes = 0;
        bool m_pinned = false;
    };

    explicit ByteLruCache(std::uint64_t budgetBytes)
        : m_state(std::make_shared<State>(budgetBytes))
    {
    }

    [[nodiscard]] Handle findAndPin(const Key& key)
    {
        std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->entries.find(key);
        if (found == m_state->entries.end()) {
            ++m_state->metrics.misses;
            return {};
        }
        ++m_state->metrics.hits;
        touch(*m_state, found->second);
        pin(*m_state, found->second);
        return Handle(m_state, key, found->second.value, found->second.bytes);
    }

    [[nodiscard]] Handle insertAndPin(
        Key key, std::shared_ptr<const Value> value, std::uint64_t bytes)
    {
        if (!value) {
            throw std::invalid_argument("cannot cache a null value");
        }
        std::scoped_lock lock(m_state->mutex);
        const auto existing = m_state->entries.find(key);
        if (existing != m_state->entries.end()) {
            ++m_state->metrics.hits;
            touch(*m_state, existing->second);
            pin(*m_state, existing->second);
            return Handle(m_state, std::move(key), existing->second.value,
                existing->second.bytes);
        }
        if (bytes > m_state->metrics.budgetBytes) {
            throw CacheBudgetExceeded("cache entry exceeds the entire byte budget");
        }
        // If pinned entries alone already leave no room, the insert cannot
        // succeed regardless of eviction; fail before discarding unpinned data
        // that the doomed insert would otherwise evict for nothing.
        if (m_state->metrics.pinnedBytes + bytes > m_state->metrics.budgetBytes) {
            throw CacheBudgetExceeded("cache budget is occupied by pinned entries");
        }
        evictFor(*m_state, bytes);

        m_state->lru.push_front(key);
        Entry entry{std::move(value), bytes, 1, m_state->lru.begin()};
        m_state->metrics.residentBytes += bytes;
        m_state->metrics.pinnedBytes += bytes;
        const auto [inserted, success] = m_state->entries.emplace(key, std::move(entry));
        if (!success) {
            throw std::logic_error("cache insertion failed unexpectedly");
        }
        return Handle(m_state, std::move(key), inserted->second.value, bytes);
    }

    [[nodiscard]] bool erase(const Key& key)
    {
        std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->entries.find(key);
        if (found == m_state->entries.end() || found->second.pinCount != 0) {
            return false;
        }
        eraseEntry(*m_state, found);
        return true;
    }

    [[nodiscard]] bool setBudget(std::uint64_t budgetBytes)
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->metrics.budgetBytes = budgetBytes;
        evictFor(*m_state, 0);
        return m_state->metrics.residentBytes <= budgetBytes;
    }

    void clearUnpinned()
    {
        std::scoped_lock lock(m_state->mutex);
        auto current = m_state->entries.begin();
        while (current != m_state->entries.end()) {
            if (current->second.pinCount == 0) {
                const auto doomed = current++;
                eraseEntry(*m_state, doomed);
            } else {
                ++current;
            }
        }
    }

    [[nodiscard]] CacheMetrics metrics() const
    {
        std::scoped_lock lock(m_state->mutex);
        return m_state->metrics;
    }

private:
    struct Entry {
        std::shared_ptr<const Value> value;
        std::uint64_t bytes = 0;
        std::uint64_t pinCount = 0;
        typename std::list<Key>::iterator lruPosition;
    };

    using EntryMap = std::unordered_map<Key, Entry, Hash>;

    struct State {
        explicit State(std::uint64_t budgetBytes)
        {
            metrics.budgetBytes = budgetBytes;
        }

        mutable std::mutex mutex;
        CacheMetrics metrics;
        std::list<Key> lru;
        EntryMap entries;
    };

    static void touch(State& state, Entry& entry)
    {
        state.lru.splice(state.lru.begin(), state.lru, entry.lruPosition);
        entry.lruPosition = state.lru.begin();
    }

    static void pin(State& state, Entry& entry)
    {
        if (entry.pinCount == 0) {
            state.metrics.pinnedBytes += entry.bytes;
        }
        ++entry.pinCount;
    }

    static void eraseEntry(State& state, typename EntryMap::iterator entry)
    {
        state.metrics.residentBytes -= entry->second.bytes;
        state.lru.erase(entry->second.lruPosition);
        state.entries.erase(entry);
        ++state.metrics.evictions;
    }

    static void evictFor(State& state, std::uint64_t incomingBytes)
    {
        while (state.metrics.residentBytes + incomingBytes > state.metrics.budgetBytes) {
            bool evicted = false;
            auto candidate = state.lru.rbegin();
            while (candidate != state.lru.rend()) {
                const auto found = state.entries.find(*candidate);
                if (found != state.entries.end() && found->second.pinCount == 0) {
                    eraseEntry(state, found);
                    evicted = true;
                    break;
                }
                ++candidate;
            }
            if (!evicted) {
                break;
            }
        }
    }

    std::shared_ptr<State> m_state;
};

} // namespace amrvis
