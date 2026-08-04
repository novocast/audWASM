#pragma once

// Owns level-0 waveform bins per channel and serves WaveformView (M04 "API"). Per-channel bins
// are populated incrementally by WaveformAnalyzer as chunks decode; mono-sum and mid/side bins are
// computed lazily on first request, straight from the AudioBuffer's PCM — not derived from the
// per-channel bins, because min(L+R)/2 != (min(L)+min(R))/2 (M04 "Stereo presentation modes").

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "../util/audio_buffer.hpp"
#include "../util/audio_types.hpp"
#include "../util/result.hpp"
#include "waveform_bin.hpp"

namespace aud::waveform {

enum class ChannelSelector : std::uint8_t {
    PerChannel,  // engine-native per-channel bins
    MonoSum,     // (sum of all channels) / channelCount
    MidSide,     // stereo only: M = (L+R)/2, S = (L-R)/2
};

struct WaveformRequest {
    ChannelSelector channels  = ChannelSelector::PerChannel;
    FrameRange      range     = {};
    std::uint32_t   binCount  = 0;  // desired output bins (~= pixel width)
};

struct WaveformView {
    const WaveformBin* data         = nullptr;  // [channels][binCount], contiguous per channel
    std::uint32_t      channels     = 0;
    std::uint32_t      binCount     = 0;
    std::uint32_t      framesPerBin = 0;
    bool                isComplete  = false;  // false if the requested range isn't fully decoded yet
};

class WaveformStore {
public:
    // Resets to `channelCount` empty per-channel bin vectors and clears any cached derived
    // variants and completion state. Called once from WaveformAnalyzer::begin().
    void reset(ChannelIndex channelCount);

    // Reduces `chunkSamples` into level-0 bins and appends them to channel `ch`'s bin vector.
    // `chunkSamples` must be a bin-aligned AudioBuffer chunk (see waveform_bin.hpp's static_assert)
    // except possibly the very last one, which may be shorter.
    void appendChunk(ChannelIndex ch, std::span<const Sample> chunkSamples);

    void markComplete() noexcept { m_complete = true; }
    [[nodiscard]] bool isComplete() const noexcept { return m_complete; }

    [[nodiscard]] ChannelIndex channelCount() const noexcept { return static_cast<ChannelIndex>(m_perChannelBins.size()); }

    // Raw level-0 bins for one channel, in frame order.
    [[nodiscard]] std::span<const WaveformBin> bins(ChannelIndex ch) const noexcept;

    // Computed on first call from `buffer`'s PCM, cached afterward. `buffer` must be the same
    // buffer (same frame data) `this` was populated from. Errors if MidSide is requested on a
    // non-stereo buffer.
    [[nodiscard]] Result<std::span<const WaveformBin>> monoSumBins(const AudioBuffer& buffer);
    [[nodiscard]] Result<std::span<const WaveformBin>> midBins(const AudioBuffer& buffer);
    [[nodiscard]] Result<std::span<const WaveformBin>> sideBins(const AudioBuffer& buffer);

    // Aggregates level-0 bins (or a lazily-computed variant) down to request.binCount output bins
    // over request.range. In this milestone only level 0 exists, so aggregation is a simple loop
    // over the covering base bins; M05 replaces this with real pyramid-level selection. The
    // returned view points into scratch storage owned by `this`, valid until the next query() call.
    [[nodiscard]] Result<WaveformView> query(const WaveformRequest& request, const AudioBuffer* buffer);

private:
    // Fills m_mid and m_side together (one pass over the PCM computes both). No-op if already
    // cached. Errors (leaving both uncached) if `buffer` isn't exactly stereo.
    [[nodiscard]] Result<void> ensureMidSide(const AudioBuffer& buffer);

    std::vector<std::vector<WaveformBin>> m_perChannelBins;  // planar: [channel][binIndex]
    std::optional<std::vector<WaveformBin>> m_monoSum;
    std::optional<std::vector<WaveformBin>> m_mid;
    std::optional<std::vector<WaveformBin>> m_side;
    bool m_complete = false;

    std::vector<WaveformBin> m_queryScratch;  // reused output buffer for query()
};

}  // namespace aud::waveform
