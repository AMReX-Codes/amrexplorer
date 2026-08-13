#pragma once

#include <amrexplorer/cache/CacheMetrics.hpp>

#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

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

    // Like findAndPin but records no hit or miss. Used for the double-checked
    // second lookup a caller performs after taking a heavier lock: the logical
    // lookup was already counted by the first findAndPin, so counting again
    // would double-count the miss (and record a spurious miss+hit for a
    // key a racing thread inserted in between).
    [[nodiscard]] Handle peekAndPin(const Key& key)
    {
        std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->entries.find(key);
        if (found == m_state->entries.end()) {
            return {};
        }
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
        // Declared before the lock so it is destroyed after it: the payloads
        // this insert evicts are freed with the mutex already released.
        std::vector<std::shared_ptr<const Value>> doomed;
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
        evictFor(*m_state, bytes, doomed);

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
        std::vector<std::shared_ptr<const Value>> doomed;
        std::scoped_lock lock(m_state->mutex);
        const auto found = m_state->entries.find(key);
        if (found == m_state->entries.end() || found->second.pinCount != 0) {
            return false;
        }
        eraseEntry(*m_state, found, doomed);
        ++m_state->metrics.clears;
        return true;
    }

    [[nodiscard]] bool setBudget(std::uint64_t budgetBytes)
    {
        std::vector<std::shared_ptr<const Value>> doomed;
        std::scoped_lock lock(m_state->mutex);
        m_state->metrics.budgetBytes = budgetBytes;
        evictFor(*m_state, 0, doomed);
        return m_state->metrics.residentBytes <= budgetBytes;
    }

    void clearUnpinned()
    {
        // A full clear frees every unpinned payload; without this they would
        // all be released serially with the mutex held.
        std::vector<std::shared_ptr<const Value>> doomed;
        std::scoped_lock lock(m_state->mutex);
        auto current = m_state->entries.begin();
        while (current != m_state->entries.end()) {
            if (current->second.pinCount == 0) {
                const auto entry = current++;
                eraseEntry(*m_state, entry, doomed);
                ++m_state->metrics.clears;
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

    // Removes an entry's storage and accounting. The caller records the
    // reason (capacity eviction vs. explicit clear) in the matching counter.
    //
    // The payload is handed to `doomed` rather than released here. Every caller
    // holds the cache mutex, and a cached block is megabytes: freeing it inline
    // makes a panning burst's evictions stall every concurrent findAndPin and
    // Handle::release behind a deallocator. Callers declare `doomed` before
    // they take the lock, so it outlives the lock and the memory is returned
    // after the mutex is free.
    //
    // The map entry is retired before `doomed` grows, because push_back
    // allocates: a throw between erasing the LRU node and erasing the map entry
    // would leave a live entry holding a freed lruPosition (which the next
    // touch() splices) and a null value (which the returned Handle
    // dereferences). Ordered this way the only cost of a failed push_back is
    // that this one payload is freed under the mutex instead of after it.
    static void eraseEntry(State& state, typename EntryMap::iterator entry,
        std::vector<std::shared_ptr<const Value>>& doomed)
    {
        auto value = std::move(entry->second.value);
        state.metrics.residentBytes -= entry->second.bytes;
        state.lru.erase(entry->second.lruPosition);
        state.entries.erase(entry);
        doomed.push_back(std::move(value));
    }

    // Single tail-to-head sweep of the LRU list, evicting unpinned entries
    // until the incoming bytes fit or the list is exhausted. Each list node is
    // examined at most once (O(n)); the previous version restarted from the
    // tail after every eviction, making it O(k*n) with a pinned-heavy tail.
    static void evictFor(State& state, std::uint64_t incomingBytes,
        std::vector<std::shared_ptr<const Value>>& doomed)
    {
        auto it = state.lru.end();
        while (it != state.lru.begin()
            && state.metrics.residentBytes + incomingBytes
                > state.metrics.budgetBytes) {
            const auto candidate = std::prev(it);
            const auto found = state.entries.find(*candidate);
            if (found != state.entries.end() && found->second.pinCount == 0) {
                // eraseEntry removes *candidate from the list (invalidating
                // `candidate`); `it` points past it and stays valid, so the
                // next std::prev(it) is the node that preceded the erased one.
                eraseEntry(state, found, doomed);
                ++state.metrics.evictions;
            } else {
                // Keep a pinned (or already-gone) entry and move toward head.
                it = candidate;
            }
        }
    }

    std::shared_ptr<State> m_state;
};

} // namespace amrvis
