#pragma once

// Single-pass 100 ms sub-block accumulator. M08's decision: momentary (400 ms/4 blocks) and
// short-term (3 s/30 blocks) are both derived from one running set of 100 ms sub-block mean
// squares rather than two independent sliding windows — one pass, O(1) extra memory, no
// off-by-one risk from maintaining two window implementations.
//
// This class only produces the *combined, channel-weighted* mean square per sub-block (BS.1770's
// Σ_ch G_ch·z_ch, pre-log) — that single scalar is all momentary/short-term/gating/LRA need
// downstream; per-channel mean squares are never retained past the sub-block they belong to.

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "../../util/audio_types.hpp"

namespace aud::loudness {

// Emitted once per completed 100 ms sub-block: the channel-weighted mean square (linear, not dB).
using SubBlockCallback = std::function<void(double weightedMeanSquare)>;

class BlockAccumulator {
public:
    void begin(SampleRate sampleRate, std::span<const double> channelWeights);

    // `kWeightedChannels[c]` holds this chunk's K-weighted samples (double) for channel c, one
    // span per entry in the `channelWeights` passed to begin(), all the same length. Slices into
    // 100 ms sub-blocks (carrying any remainder across calls) and invokes `onSubBlock` for each
    // one completed by this call.
    void process(std::span<const std::span<const double>> kWeightedChannels, const SubBlockCallback& onSubBlock);

    // Any partial trailing sub-block (< 100 ms) is simply dropped — the standard BS.1770
    // convention, matching how a too-short final block is excluded from gating everywhere else.
    void finish() noexcept {}

    [[nodiscard]] std::size_t subBlockFrameCount() const noexcept { return m_subBlockFrames; }

private:
    SampleRate          m_sampleRate    = 0;
    std::vector<double> m_weights;
    std::size_t         m_subBlockFrames = 0;

    // Per-channel carry of K-weighted samples not yet enough to complete a sub-block.
    std::vector<std::vector<double>> m_carry;
};

// O(1)-per-push sliding sum over the last `window` pushed sub-block values, via a ring buffer —
// the shared building block for momentary (window=4), the meter's short-term (window=30), and
// LRA's short-term (window=30, sampled every 10th completed push for its 1 s hop; see lra.hpp).
// Reports the window's mean once `window` values have been pushed, and on every push thereafter.
class SlidingWindowSum {
public:
    explicit SlidingWindowSum(std::size_t window) : m_window(window), m_ring(window, 0.0) {}

    // Returns true and writes the window mean to `outMean` once the window first fills; false
    // (outMean untouched) while still warming up.
    bool push(double value, double& outMean) noexcept {
        const double old = m_ring[m_pos];
        m_ring[m_pos]     = value;
        m_pos             = (m_pos + 1) % m_window;
        m_sum += value - old;
        if (m_count < m_window) ++m_count;
        ++m_totalPushes;

        if (m_count >= m_window) {
            outMean = m_sum / static_cast<double>(m_window);
            return true;
        }
        return false;
    }

    // Total pushes ever made (unbounded, unlike the capped `m_count` used to detect warm-up) —
    // used by callers that need to sub-sample this window's output at a coarser hop (e.g. LRA's
    // 1 s hop over a 3 s/30-block window; see loudness_analyzer.cpp).
    [[nodiscard]] std::size_t pushCount() const noexcept { return m_totalPushes; }

private:
    std::size_t         m_window;
    std::vector<double> m_ring;
    std::size_t         m_pos          = 0;
    double              m_sum          = 0.0;
    std::size_t         m_count        = 0;
    std::size_t         m_totalPushes  = 0;
};

}  // namespace aud::loudness
