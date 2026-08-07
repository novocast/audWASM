#pragma once

// Bounded "worst N" accumulator (M11 "Event list capping"): a badly clipped file can produce
// hundreds of thousands of events; storing and rendering all of them is pointless and slow, but
// silently truncating (e.g. "first 10000") would hide the actual worst offenders behind whatever
// happened to occur early in the file. A fixed-capacity min-heap keyed by severity keeps exactly
// the `capacity` most-severe items ever offered, in O(log capacity) per push, while a plain counter
// tracks how many were offered in total so callers can report the honest count alongside the
// (possibly much smaller) stored list — never "12483 events" quietly rendered as if it were 10000.

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace aud::util {

// `KeyFn` maps a T to a totally-ordered severity (double, typically); larger = more severe = more
// likely to survive capping. Ties are broken arbitrarily (insertion order is not preserved).
template <class T, class KeyFn>
class BoundedTopN {
public:
    BoundedTopN(std::size_t capacity, KeyFn keyFn) : m_capacity(capacity), m_keyFn(std::move(keyFn)) {}

    // O(log capacity): either grows the heap (while under capacity) or, once full, discards the
    // new item unless it's more severe than the weakest item currently kept — in which case that
    // weakest item is evicted.
    void push(T item) {
        ++m_totalOffered;
        if (m_capacity == 0) return;

        if (m_heap.size() < m_capacity) {
            m_heap.push_back(std::move(item));
            std::push_heap(m_heap.begin(), m_heap.end(), heapOrder());
            return;
        }
        if (m_keyFn(item) > m_keyFn(m_heap.front())) {
            std::pop_heap(m_heap.begin(), m_heap.end(), heapOrder());
            m_heap.back() = std::move(item);
            std::push_heap(m_heap.begin(), m_heap.end(), heapOrder());
        }
    }

    // Every item ever pushed, capped or not — the exact count callers must keep reporting even
    // after capping drops the stored list below it (M11: "Never silently truncate; always show the
    // true count").
    [[nodiscard]] std::size_t totalOffered() const noexcept { return m_totalOffered; }

    [[nodiscard]] std::size_t size() const noexcept { return m_heap.size(); }
    [[nodiscard]] bool        capped() const noexcept { return m_totalOffered > m_heap.size(); }

    // Drains the kept items, most severe first — the ordering callers generally want for display
    // ("showing worst N"). Leaves this accumulator empty; totalOffered() is unaffected.
    [[nodiscard]] std::vector<T> extractSorted() {
        std::vector<T> out = std::move(m_heap);
        std::sort(out.begin(), out.end(), [this](const T& a, const T& b) { return m_keyFn(a) > m_keyFn(b); });
        m_heap.clear();
        return out;
    }

private:
    // std::push_heap/pop_heap build a *max*-heap under `Compare`; using `>` here makes the
    // container's "top" (m_heap.front()) the *minimum*-key item, i.e. the weakest of the kept
    // items — exactly the one we want to evict first when something more severe arrives.
    [[nodiscard]] auto heapOrder() const {
        return [this](const T& a, const T& b) { return m_keyFn(a) > m_keyFn(b); };
    }

    std::size_t    m_capacity;
    KeyFn          m_keyFn;
    std::vector<T> m_heap;
    std::size_t    m_totalOffered = 0;
};

}  // namespace aud::util
