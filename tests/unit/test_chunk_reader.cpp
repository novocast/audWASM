#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <random>
#include <vector>

#include "../../engine/util/audio_buffer.hpp"
#include "../../engine/util/chunk_reader.hpp"

using aud::AudioBuffer;
using aud::ChunkReader;
using aud::FrameRange;
using aud::Sample;

namespace {

AudioBuffer makeBufferFrom(const std::vector<Sample>& mono) {
    auto result = AudioBuffer::create(44100, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();
    std::vector<std::span<const Sample>> planar{mono};
    REQUIRE(buffer.append(planar, mono.size()).has_value());
    return buffer;
}

}  // namespace

TEST_CASE("ChunkReader::read is sequential and stops at end of buffer", "[chunk_reader]") {
    std::vector<Sample> mono(1000);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        mono[i] = static_cast<float>(i);
    }
    auto buffer = makeBufferFrom(mono);

    ChunkReader        reader(buffer, 0);
    std::vector<Sample> out(300);

    REQUIRE(reader.read(out) == 300);
    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[299] == 299.0f);

    REQUIRE(reader.read(out) == 300);
    REQUIRE(out[0] == 300.0f);

    REQUIRE(reader.read(out) == 300);
    REQUIRE(reader.read(out) == 100);  // only 100 frames left
    REQUIRE(reader.atEnd());
    REQUIRE(reader.read(out) == 0);
}

TEST_CASE("forEachWindow across chunk boundaries matches a naive monolithic implementation", "[chunk_reader]") {
    std::mt19937 rng(0xC0FFEE);

    // A signal spanning a bit over 2 chunks so windows are forced to straddle boundaries.
    const std::size_t   totalFrames = (AudioBuffer::kChunkFrames * 2) + 500;
    std::vector<Sample> mono(totalFrames);
    std::uniform_real_distribution<float> valueDist(-1.0f, 1.0f);
    for (auto& s : mono) {
        s = valueDist(rng);
    }
    auto buffer = makeBufferFrom(mono);

    std::uniform_int_distribution<std::size_t> hopDist(1, 4096);
    std::uniform_int_distribution<std::size_t> sizeDist(1, 4096);

    std::vector<Sample> scratch;

    constexpr int kTrials = 10'000;
    for (int trial = 0; trial < kTrials; ++trial) {
        const std::size_t hop  = hopDist(rng);
        const std::size_t size = sizeDist(rng);
        if (size > totalFrames) {
            continue;
        }

        std::size_t windowsChecked = 0;
        forEachWindow(buffer, 0, hop, size, scratch, [&](std::span<const Sample> window, aud::FrameIndex start) {
            REQUIRE(window.size() == size);
            const auto* expected = mono.data() + static_cast<std::size_t>(start);
            REQUIRE(std::equal(window.begin(), window.end(), expected));
            ++windowsChecked;
        });

        // Sanity: the number of windows visited matches the naive formula.
        const std::size_t expectedWindows = (totalFrames - size) / hop + 1;
        REQUIRE(windowsChecked == expectedWindows);
    }
}

TEST_CASE("forEachWindow with size larger than the buffer produces no windows", "[chunk_reader]") {
    std::vector<Sample> mono(10, 1.0f);
    auto                buffer = makeBufferFrom(mono);

    std::vector<Sample> scratch;
    std::size_t         calls = 0;
    forEachWindow(buffer, 0, 1, 20, scratch, [&](std::span<const Sample>, aud::FrameIndex) { ++calls; });
    REQUIRE(calls == 0);
}
