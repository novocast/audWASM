#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <random>
#include <vector>

#include "../../engine/util/audio_buffer.hpp"

using aud::AudioBuffer;
using aud::FrameRange;
using aud::Sample;

TEST_CASE("AudioBuffer::create rejects invalid arguments", "[audio_buffer]") {
    REQUIRE_FALSE(AudioBuffer::create(0, 2).has_value());
    REQUIRE_FALSE(AudioBuffer::create(44100, 0).has_value());
}

TEST_CASE("AudioBuffer starts empty", "[audio_buffer]") {
    auto result = AudioBuffer::create(44100, 2);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();
    REQUIRE(buffer.frameCount() == 0);
    REQUIRE(buffer.chunkCount() == 0);
}

TEST_CASE("AudioBuffer append then read round-trips exactly", "[audio_buffer]") {
    auto bufferResult = AudioBuffer::create(44100, 2);
    REQUIRE(bufferResult.has_value());
    auto buffer = std::move(bufferResult).value();

    constexpr std::size_t kFrames = 10'000;
    std::vector<Sample>   left(kFrames), right(kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        left[i]  = static_cast<float>(i);
        right[i] = -static_cast<float>(i);
    }

    std::vector<std::span<const Sample>> planar{left, right};
    REQUIRE(buffer.append(planar, kFrames).has_value());
    REQUIRE(buffer.frameCount() == static_cast<aud::FrameIndex>(kFrames));

    std::vector<Sample> readLeft(kFrames), readRight(kFrames);
    REQUIRE(buffer.read(0, FrameRange{0, static_cast<aud::FrameIndex>(kFrames)}, readLeft).has_value());
    REQUIRE(buffer.read(1, FrameRange{0, static_cast<aud::FrameIndex>(kFrames)}, readRight).has_value());

    REQUIRE(readLeft == left);
    REQUIRE(readRight == right);
}

TEST_CASE("AudioBuffer append spanning multiple chunk boundaries stitches correctly", "[audio_buffer]") {
    auto bufferResult = AudioBuffer::create(48000, 1);
    REQUIRE(bufferResult.has_value());
    auto buffer = std::move(bufferResult).value();

    // A bit over 2 chunks, appended in small irregular pieces to exercise boundary crossing.
    const std::size_t totalFrames = (AudioBuffer::kChunkFrames * 2) + 123;
    std::vector<Sample> source(totalFrames);
    for (std::size_t i = 0; i < totalFrames; ++i) {
        source[i] = std::sin(static_cast<float>(i) * 0.001f);
    }

    std::mt19937                    rng(1234);
    std::uniform_int_distribution<> pieceSizeDist(1, 4096);

    std::size_t written = 0;
    while (written < totalFrames) {
        std::size_t piece = std::min<std::size_t>(pieceSizeDist(rng), totalFrames - written);
        std::span<const Sample>              chunk(source.data() + written, piece);
        std::vector<std::span<const Sample>> planar{chunk};
        REQUIRE(buffer.append(planar, piece).has_value());
        written += piece;
    }

    REQUIRE(buffer.frameCount() == static_cast<aud::FrameIndex>(totalFrames));
    REQUIRE(buffer.chunkCount() == 3);

    std::vector<Sample> readBack(totalFrames);
    REQUIRE(buffer.read(0, FrameRange{0, static_cast<aud::FrameIndex>(totalFrames)}, readBack).has_value());
    REQUIRE(readBack == source);
}

TEST_CASE("AudioBuffer::read rejects out-of-bounds ranges", "[audio_buffer]") {
    auto bufferResult = AudioBuffer::create(44100, 1);
    REQUIRE(bufferResult.has_value());
    auto buffer = std::move(bufferResult).value();

    std::vector<Sample>                  data(10, 1.0f);
    std::vector<std::span<const Sample>> planar{data};
    REQUIRE(buffer.append(planar, 10).has_value());

    std::vector<Sample> out(5);
    REQUIRE_FALSE(buffer.read(0, FrameRange{5, 20}, out).has_value());
    REQUIRE_FALSE(buffer.read(1, FrameRange{0, 5}, out).has_value());
}

TEST_CASE("AudioBuffer never allocates a chunk larger than kChunkFrames * sizeof(Sample)", "[audio_buffer]") {
    // Indirect check: append exactly kChunkFrames+1 frames and confirm chunkCount() == 2, i.e. the
    // implementation split storage rather than growing a single allocation.
    auto bufferResult = AudioBuffer::create(44100, 1);
    REQUIRE(bufferResult.has_value());
    auto buffer = std::move(bufferResult).value();

    const std::size_t              frames = AudioBuffer::kChunkFrames + 1;
    std::vector<Sample>            data(frames, 0.5f);
    std::vector<std::span<const Sample>> planar{data};
    REQUIRE(buffer.append(planar, frames).has_value());
    REQUIRE(buffer.chunkCount() == 2);
}
