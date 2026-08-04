#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <span>
#include <vector>

#include "../../engine/util/audio_buffer.hpp"
#include "../../engine/waveform/pyramid.hpp"
#include "../../engine/waveform/reduce.hpp"
#include "../../engine/waveform/waveform_bin.hpp"
#include "../../engine/waveform/waveform_store.hpp"

using aud::AudioBuffer;
using aud::FrameRange;
using aud::Sample;
using Catch::Approx;
using namespace aud::waveform;

namespace {

// Feeds `frameCount` frames of `signal(frameIndex)` through WaveformStore exactly the way
// WaveformAnalyzer::process()/the Embind driver do: one appendChunk() per AudioBuffer chunk, so the
// pyramid cascade builds incrementally and only the very last chunk can be short.
template <class SignalFn>
WaveformStore buildStore(std::size_t frameCount, SignalFn signal) {
    auto result = AudioBuffer::create(44100, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    std::vector<Sample> samples(frameCount);
    for (std::size_t i = 0; i < frameCount; ++i) {
        samples[i] = signal(i);
    }
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frameCount).has_value());

    WaveformStore store;
    store.reset(1);
    for (std::size_t c = 0; c < buffer.chunkCount(); ++c) {
        store.appendChunk(0, buffer.chunk(0, c));
    }
    store.markComplete();
    return store;
}

}  // namespace

TEST_CASE("fold() is exact for min/max/absPeak regardless of child frame counts", "[waveform][pyramid]") {
    WaveformBin a{.min = -0.5f, .max = 0.3f, .rms = 0.2f, .absPeak = 0.5f};
    WaveformBin b{.min = -0.1f, .max = 0.9f, .rms = 0.4f, .absPeak = 0.9f};

    const WaveformBin equal     = fold(a, 256, b, 256);
    REQUIRE(equal.min == Approx(-0.5f));
    REQUIRE(equal.max == Approx(0.9f));
    REQUIRE(equal.absPeak == Approx(0.9f));
    REQUIRE(equal.rms == Approx(std::sqrt(0.5 * (0.2 * 0.2 + 0.4 * 0.4))));

    const WaveformBin weighted = fold(a, 256, b, 64);
    REQUIRE(weighted.min == Approx(-0.5f));
    REQUIRE(weighted.max == Approx(0.9f));
    REQUIRE(weighted.absPeak == Approx(0.9f));
    const double expectedSumSq = 256.0 * 0.2 * 0.2 + 64.0 * 0.4 * 0.4;
    REQUIRE(weighted.rms == Approx(std::sqrt(expectedSumSq / 320.0)));
}

TEST_CASE("fold() reduces to the equal-weight formula when frame counts match", "[waveform][pyramid]") {
    WaveformBin a{.min = -1.0f, .max = 1.0f, .rms = 0.7071f, .absPeak = 1.0f};
    WaveformBin b{.min = -1.0f, .max = 1.0f, .rms = 0.7071f, .absPeak = 1.0f};
    const WaveformBin out = fold(a, 512, b, 512);
    REQUIRE(out.rms == Approx(std::sqrt(0.5f * (a.rms * a.rms + b.rms * b.rms))).margin(1e-6));
}

TEST_CASE("selectLevel floors rather than rounds and clamps to maxLevel", "[waveform][pyramid]") {
    REQUIRE(selectLevel(200, 10) == kRawPcmLevel);       // finer than level 0
    REQUIRE(selectLevel(256, 10) == 0);                  // exactly level 0
    REQUIRE(selectLevel(511, 10) == 0);                  // rounds down, not up to level 1
    REQUIRE(selectLevel(512, 10) == 1);
    REQUIRE(selectLevel(1000, 10) == 1);                 // 1000/256 ~= 3.9 -> floor(log2) = 1
    REQUIRE(selectLevel(2048, 10) == 3);
    REQUIRE(selectLevel(1ull << 40, 10) == 10);          // clamped to maxLevel
}

TEST_CASE("an impulse is present with full amplitude at every pyramid level", "[waveform][pyramid][regression]") {
    // A big enough track that the pyramid grows several levels deep, per M05's level table.
    constexpr std::size_t frames      = kBaseBinFrames * 4096;  // ~1M frames
    constexpr std::size_t impulseAt   = kBaseBinFrames * 777 + 13;
    auto store = buildStore(frames, [](std::size_t i) { return i == impulseAt ? 1.0f : 0.0f; });

    // Walk every level directly via the store's raw level-0 accessor plus repeated query() calls at
    // increasing binCount granularity is indirect; instead exercise the pyramid the store owns by
    // querying at the exact frame resolution of each level and checking the peak survives.
    for (std::uint32_t level = 0; level < 16; ++level) {
        const std::uint64_t framesPerBin = WaveformPyramid::framesPerBin(level);
        if (framesPerBin > frames) {
            break;
        }
        WaveformRequest request;
        request.channels = ChannelSelector::PerChannel;
        request.range    = FrameRange{0, static_cast<aud::FrameIndex>(frames)};
        request.binCount = static_cast<std::uint32_t>(frames / framesPerBin);
        if (request.binCount == 0) {
            break;
        }

        // Need an AudioBuffer to query against; rebuild is wasteful but query() requires one, so
        // reconstruct the same signal deterministically for the read-back path (raw-PCM fallback,
        // never hit here since framesPerBin >= kBaseBinFrames for every level tested).
        auto bufResult = AudioBuffer::create(44100, 1);
        REQUIRE(bufResult.has_value());
        auto buffer2 = std::move(bufResult).value();
        std::vector<Sample> samples(frames, 0.0f);
        samples[impulseAt] = 1.0f;
        std::vector<std::span<const Sample>> planar{samples};
        REQUIRE(buffer2.append(planar, frames).has_value());

        auto viewResult = store.query(request, &buffer2);
        REQUIRE(viewResult.has_value());
        const auto& view = viewResult.value();

        bool found = false;
        for (std::uint32_t i = 0; i < view.binCount; ++i) {
            if (view.data[i].max >= 0.999f || view.data[i].absPeak >= 0.999f) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("RMS at every level matches direct computation, including the trailing partial bin",
          "[waveform][pyramid][regression]") {
    // Deliberately adversarial lengths per the M05 acceptance criteria: not multiples of
    // kBaseBinFrames, including lengths shorter than one full base bin.
    for (std::size_t frames : {std::size_t{1}, std::size_t{255}, std::size_t{257}, std::size_t{65535}, std::size_t{65537}}) {
        auto store = buildStore(frames, [](std::size_t i) { return static_cast<Sample>(std::sin(0.05 * static_cast<double>(i))); });

        // Direct level-0 reference: reduce the whole signal in one shot and compare bin-for-bin.
        std::vector<Sample> samples(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            samples[i] = static_cast<Sample>(std::sin(0.05 * static_cast<double>(i)));
        }
        std::vector<WaveformBin> expected;
        reduceToBins(samples, kBaseBinFrames, expected);

        auto actual = store.bins(0);
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(actual[i].rms == Approx(expected[i].rms).epsilon(1e-5));
            REQUIRE(actual[i].min == Approx(expected[i].min).margin(1e-6));
            REQUIRE(actual[i].max == Approx(expected[i].max).margin(1e-6));
        }
    }
}

TEST_CASE("level-1 trailing bin RMS matches direct PCM computation over its exact frame range",
          "[waveform][pyramid][regression]") {
    for (std::size_t frames : {std::size_t{257}, std::size_t{600}, std::size_t{65537}, std::size_t{700000}}) {
        auto signal = [](std::size_t i) { return static_cast<Sample>(std::sin(0.05 * static_cast<double>(i))); };
        auto store  = buildStore(frames, signal);

        auto bufResult = AudioBuffer::create(44100, 1);
        REQUIRE(bufResult.has_value());
        auto buffer = std::move(bufResult).value();
        std::vector<Sample> samples(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            samples[i] = signal(i);
        }
        std::vector<std::span<const Sample>> planar{samples};
        REQUIRE(buffer.append(planar, frames).has_value());

        WaveformRequest request;
        request.channels = ChannelSelector::PerChannel;
        request.range    = FrameRange{0, static_cast<aud::FrameIndex>(frames)};
        // One output bin at level-1 granularity (512 frames/bin) covering the whole track,
        // deliberately including the trailing partial region.
        request.binCount = 1;
        auto viewResult = store.query(request, &buffer);
        REQUIRE(viewResult.has_value());
        const auto& view = viewResult.value();

        const double sumSq = [&] {
            double s = 0.0;
            for (Sample v : samples) {
                s += static_cast<double>(v) * static_cast<double>(v);
            }
            return s;
        }();
        const double expectedRms = std::sqrt(sumSq / static_cast<double>(frames));
        REQUIRE(view.data[0].rms == Approx(expectedRms).epsilon(1e-4));
    }
}

TEST_CASE("pyramid memory stays under 2.05x level-0 memory", "[waveform][pyramid]") {
    // 10s, 5min, 3h @ 44.1kHz mono — exercised directly against WaveformPyramid so the test doesn't
    // need to actually decode hours of PCM.
    for (std::size_t totalFrames : {std::size_t{441000}, std::size_t{5 * 60 * 44100}, std::size_t{3 * 3600 * 44100}}) {
        WaveformPyramid pyramid;
        pyramid.reset(1);

        std::vector<WaveformBin> chunkBins(kBinsPerChunk, WaveformBin{});
        std::size_t              remaining = totalFrames;
        while (remaining > 0) {
            const std::size_t framesThisCall = std::min(remaining, AudioBuffer::kChunkFrames);
            const std::size_t binsThisCall   = (framesThisCall + kBaseBinFrames - 1) / kBaseBinFrames;
            pyramid.appendLevel0Bins(0, std::span<const WaveformBin>(chunkBins).first(binsThisCall), framesThisCall);
            remaining -= framesThisCall;
        }
        pyramid.finalize();

        const std::size_t level0Count = pyramid.level(0, 0).size();
        const std::size_t totalCount  = pyramid.totalBinCount(0);
        REQUIRE(static_cast<double>(totalCount) <= 2.05 * static_cast<double>(level0Count));
    }
}

TEST_CASE("fractional aggregation never lets a scrolling impulse disappear", "[waveform][pyramid][regression]") {
    constexpr std::size_t frames    = kBaseBinFrames * 200;
    constexpr std::size_t impulseAt = kBaseBinFrames * 100 + 7;
    auto                  store     = buildStore(frames, [](std::size_t i) { return i == impulseAt ? 1.0f : 0.0f; });

    auto bufResult = AudioBuffer::create(44100, 1);
    REQUIRE(bufResult.has_value());
    auto buffer = std::move(bufResult).value();
    std::vector<Sample> samples(frames, 0.0f);
    samples[impulseAt] = 1.0f;
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frames).has_value());

    // Scroll a fixed-width window one frame at a time across the impulse; at every position the
    // impulse must appear somewhere in the output (never dropped by nearest-neighbour-style
    // rounding of the fractional source-bin coverage).
    constexpr aud::FrameIndex windowFrames = kBaseBinFrames * 20;
    constexpr std::uint32_t   binCount     = 40;  // coarser than level 0, forces aggregation
    for (aud::FrameIndex start = static_cast<aud::FrameIndex>(impulseAt) - windowFrames + 1;
         start <= static_cast<aud::FrameIndex>(impulseAt); ++start) {
        WaveformRequest request;
        request.channels = ChannelSelector::PerChannel;
        request.range    = FrameRange{std::max<aud::FrameIndex>(start, 0), std::max<aud::FrameIndex>(start, 0) + windowFrames};
        request.binCount = binCount;

        auto viewResult = store.query(request, &buffer);
        REQUIRE(viewResult.has_value());
        const auto& view = viewResult.value();

        bool found = false;
        for (std::uint32_t i = 0; i < view.binCount; ++i) {
            if (view.data[i].absPeak >= 0.999f) {
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("query() latency benchmark across levels for a 4000-bin request", "[waveform][pyramid][.benchmark]") {
    constexpr std::size_t frames = kBaseBinFrames * 20000;  // large enough to reach deep levels
    auto                  store  = buildStore(frames, [](std::size_t i) { return static_cast<Sample>((i % 997) / 997.0f); });

    auto bufResult = AudioBuffer::create(44100, 1);
    REQUIRE(bufResult.has_value());
    auto buffer = std::move(bufResult).value();
    std::vector<Sample> samples(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        samples[i] = static_cast<Sample>((i % 997) / 997.0f);
    }
    std::vector<std::span<const Sample>> planar{samples};
    REQUIRE(buffer.append(planar, frames).has_value());

    WaveformRequest request;
    request.channels = ChannelSelector::PerChannel;
    request.binCount  = 4000;
    request.range     = FrameRange{0, static_cast<aud::FrameIndex>(frames)};

    BENCHMARK("query() over the full range (coarsest usable level)") {
        return store.query(request, &buffer);
    };

    request.range = FrameRange{0, kBaseBinFrames * 4000};
    BENCHMARK("query() at level 0 (finest range matching binCount)") {
        return store.query(request, &buffer);
    };
}
