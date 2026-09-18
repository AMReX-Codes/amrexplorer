#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace amrvis::qt {

// Completed user actions only. Rendering, resize and cosmetic refreshes never
// enter this container. Values must not own datasets or rendered images.
template <typename State>
class NavigationHistory {
public:
    struct Entry {
        State before;
        State after;
    };

    void push(State before, State after)
    {
        if (before == after) {
            return;
        }
        m_entries.resize(m_cursor);
        m_entries.push_back({std::move(before), std::move(after)});
        if (m_entries.size() > 100) {
            m_entries.erase(m_entries.begin());
        }
        m_cursor = m_entries.size();
    }
    [[nodiscard]] bool canBack() const { return m_cursor != 0; }
    [[nodiscard]] bool canForward() const { return m_cursor < m_entries.size(); }
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }
    const Entry* back() { return canBack() ? &m_entries[--m_cursor] : nullptr; }
    const Entry* forward() { return canForward() ? &m_entries[m_cursor++] : nullptr; }
    void clear() { m_entries.clear(); m_cursor = 0; }

private:
    std::vector<Entry> m_entries;
    std::size_t m_cursor = 0;
};

} // namespace amrvis::qt
