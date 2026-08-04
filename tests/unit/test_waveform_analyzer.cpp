#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

#include "../../engine/util/audio_buffer.hpp"
#include "../../engine/waveform/reduce.hpp"
#include "../../engine/waveform/waveform_analyzer.hpp"
#include "../../engine/waveform/waveform_store.hpp"

using aud::AudioBuffer;
using aud::ChannelIndex;
using aud::Sample;
using aud::waveform::kBaseBinFrames;
using aud::waveform::makeWaveformAnalyzer;
using aud::waveform::reduceToBins;
using aud::waveform::WaveformBin;
using aud::waveform::WaveformStore;

namespace {

AudioBuffer makeStereoBuffer(std::size_t frames) {
    auto result = AudioBuffer::create(44100, 2);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();

    std::vector<Sample> left(frames);
    std::vector<Sample> right(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i]  = static_cast<Sample>(std::sin(0.1 * static_cast<double>(i)));
        right[i] = static_cast<Sample>(std::cos(0.1 * static_cast<double>(i)));
    }
    std::vector<std::span<const Sample>> planar{left, right};
    REQUIRE(buffer.append(planar, frames).has_value());
    return buffer;
}

}  // namespace

TEST_CASE("WaveformAnalyzer streaming over chunks matches a direct whole-buffer reduction", "[waveform]") {
    const std::size_t frames = AudioBuffer::kChunkFrames * 2 + 500;  // forces a short trailing chunk
    auto              buffer = makeStereoBuffer(frames);

    WaveformStore store;
    // Constructed via the factory, interacted with only through the Analyzer base interface — see
    // makeWaveformAnalyzer's comment: this test TU builds with RTTI enabled (aud_core doesn't),
    // so directly naming/constructing the concrete WaveformAnalyzer here would fail to link.
    std::unique_ptr<aud::Analyzer> analyzer = makeWaveformAnalyzer(store);
    REQUIRE(analyzer->begin(aud::AudioSpec{buffer.sampleRate(), buffer.channelCount(), buffer.frameCount()})
                .has_value());

    for (std::size_t c = 0; c < buffer.chunkCount(); ++c) {
        std::vector<std::span<const Sample>> planar(buffer.channelCount());
        for (ChannelIndex ch = 0; ch < buffer.channelCount(); ++ch) {
            planar[ch] = buffer.chunk(ch, c);
        }
        aud::ChunkView view{std::span<const std::span<const Sample>>(planar), 0};
        REQUIRE(analyzer->process(view).has_value());
    }
    REQUIRE(analyzer->finish().has_value());

    REQUIRE(store.isComplete());
    REQUIRE(store.channelCount() == 2);

    for (ChannelIndex ch = 0; ch < 2; ++ch) {
        std::vector<Sample> whole(frames);
        REQUIRE(buffer.read(ch, aud::FrameRange{0, static_cast<aud::FrameIndex>(frames)}, whole).has_value());

        std::vector<WaveformBin> expected;
        reduceToBins(whole, kBaseBinFrames, expected);

        auto actual = store.bins(ch);
        REQUIRE(actual.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(actual[i].min == expected[i].min);
            REQUIRE(actual[i].max == expected[i].max);
            REQUIRE(actual[i].rms == expected[i].rms);
            REQUIRE(actual[i].absPeak == expected[i].absPeak);
        }
    }
}
