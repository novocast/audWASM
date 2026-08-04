#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <vector>

#include "../../engine/util/audio_buffer.hpp"
#include "../../engine/waveform/waveform_bin.hpp"
#include "../../engine/waveform/waveform_store.hpp"

using aud::AudioBuffer;
using aud::Sample;
using aud::waveform::ChannelSelector;
using aud::waveform::kBaseBinFrames;
using aud::waveform::WaveformRequest;
using aud::waveform::WaveformStore;
using Catch::Approx;

TEST_CASE("mono-sum bins differ from the naive (min(L)+min(R))/2 combination", "[waveform][regression]") {
    // A signal engineered so the channels' individual extremes occur at different frames than the
    // sum's extreme: L and R cancel at frame 10 (L=+1, R=-1) but agree at frame 200 (both -0.8).
    // The true minimum of the mono-sum is therefore -0.8, not the naive (-0.8 + -1.0)/2 = -0.9 that
    // combining cached per-channel bins would (incorrectly) produce — this is the exact trap M04
    // calls out under "Stereo presentation modes".
    constexpr std::size_t frames = 512;
    auto                  result = AudioBuffer::create(44100, 2);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    std::vector<Sample> left(frames, 0.0f);
    std::vector<Sample> right(frames, 0.0f);
    left[10]   = 1.0f;
    right[10]  = -1.0f;
    left[200]  = -0.8f;
    right[200] = -0.8f;
    std::vector<std::span<const Sample>> planar{left, right};
    REQUIRE(buffer.append(planar, frames).has_value());

    WaveformStore store;
    store.reset(2);
    for (std::size_t c = 0; c < buffer.chunkCount(); ++c) {
        store.appendChunk(0, buffer.chunk(0, c));
        store.appendChunk(1, buffer.chunk(1, c));
    }
    store.markComplete();

    auto monoResult = store.monoSumBins(buffer);
    REQUIRE(monoResult.has_value());
    auto monoBins = monoResult.value();

    auto leftBins  = store.bins(0);
    auto rightBins = store.bins(1);
    REQUIRE(leftBins.size() == monoBins.size());
    REQUIRE(leftBins[0].min == Approx(-0.8f));
    REQUIRE(rightBins[0].min == Approx(-1.0f));

    const float naiveMin = (leftBins[0].min + rightBins[0].min) / 2.0f;  // the trap: -0.9
    REQUIRE(monoBins[0].min == Approx(-0.8f));
    REQUIRE(naiveMin != Approx(monoBins[0].min).margin(0.001));
}

TEST_CASE("mid/side bins require exactly stereo", "[waveform]") {
    auto result = AudioBuffer::create(44100, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();
    std::vector<Sample> mono(256, 0.0f);
    std::vector<std::span<const Sample>> planar{mono};
    REQUIRE(buffer.append(planar, mono.size()).has_value());

    WaveformStore store;
    store.reset(1);
    store.appendChunk(0, buffer.chunk(0, 0));

    REQUIRE_FALSE(store.midBins(buffer).has_value());
    REQUIRE_FALSE(store.sideBins(buffer).has_value());
}

TEST_CASE("mid/side bins reveal stereo width on a genuinely wide signal", "[waveform]") {
    constexpr std::size_t frames = 256;
    auto                  result = AudioBuffer::create(44100, 2);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    // Fully out-of-phase: L = +1, R = -1 for every frame. Mid (L+R)/2 must be silent; side
    // (L-R)/2 must carry the full amplitude.
    std::vector<Sample> left(frames, 1.0f);
    std::vector<Sample> right(frames, -1.0f);
    std::vector<std::span<const Sample>> planar{left, right};
    REQUIRE(buffer.append(planar, frames).has_value());

    WaveformStore store;
    store.reset(2);
    store.appendChunk(0, buffer.chunk(0, 0));
    store.appendChunk(1, buffer.chunk(1, 0));
    store.markComplete();

    auto midResult = store.midBins(buffer);
    auto sideResult = store.sideBins(buffer);
    REQUIRE(midResult.has_value());
    REQUIRE(sideResult.has_value());
    REQUIRE(midResult.value()[0].absPeak == Approx(0.0f).margin(1e-6));
    REQUIRE(sideResult.value()[0].absPeak == Approx(1.0f));
}

TEST_CASE("query() aggregates level-0 bins down to the requested bin count", "[waveform]") {
    constexpr std::size_t frames = kBaseBinFrames * 4;
    auto                   result = AudioBuffer::create(44100, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    std::vector<Sample> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        mono[i] = static_cast<Sample>(i) / static_cast<Sample>(frames);  // monotonically increasing ramp
    }
    std::vector<std::span<const Sample>> planar{mono};
    REQUIRE(buffer.append(planar, frames).has_value());

    WaveformStore store;
    store.reset(1);
    for (std::size_t c = 0; c < buffer.chunkCount(); ++c) {
        store.appendChunk(0, buffer.chunk(0, c));
    }
    store.markComplete();

    WaveformRequest request;
    request.channels = ChannelSelector::PerChannel;
    request.range    = aud::FrameRange{0, static_cast<aud::FrameIndex>(frames)};
    request.binCount = 2;

    auto viewResult = store.query(request, &buffer);
    REQUIRE(viewResult.has_value());
    const auto& view = viewResult.value();
    REQUIRE(view.channels == 1);
    REQUIRE(view.binCount == 2);
    REQUIRE(view.isComplete);
    REQUIRE(view.data[1].max > view.data[0].max);  // second half of the ramp reaches higher
}
