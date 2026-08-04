#include <catch2/catch_test_macros.hpp>
#include <random>
#include <thread>
#include <vector>

#include "../../engine/playback/ring_buffer.hpp"

using aud::ChannelIndex;
using aud::Sample;
using aud::playback::RingBuffer;

namespace {

std::vector<std::vector<Sample>> makePlanar(ChannelIndex channels, std::size_t frames, Sample fillFrom) {
    std::vector<std::vector<Sample>> planar(channels);
    for (ChannelIndex ch = 0; ch < channels; ++ch) {
        planar[ch].resize(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            planar[ch][i] = fillFrom + static_cast<Sample>(i) + static_cast<Sample>(ch) * 100000.0f;
        }
    }
    return planar;
}

std::vector<std::span<const Sample>> constSpans(const std::vector<std::vector<Sample>>& planar) {
    std::vector<std::span<const Sample>> spans(planar.size());
    for (std::size_t ch = 0; ch < planar.size(); ++ch) {
        spans[ch] = std::span<const Sample>(planar[ch]);
    }
    return spans;
}

}  // namespace

TEST_CASE("RingBuffer capacity rounds up to a power of two", "[ring_buffer]") {
    RingBuffer ring(2, 100);
    REQUIRE(ring.capacityFrames() == 128);
}

TEST_CASE("RingBuffer round-trips written data in order", "[ring_buffer]") {
    RingBuffer ring(2, 64);
    auto       in    = makePlanar(2, 40, 0.0f);
    auto       spans = constSpans(in);

    REQUIRE(ring.write(std::span<const std::span<const Sample>>(spans), 40) == 40);
    REQUIRE(ring.framesAvailableToRead() == 40);

    std::vector<std::vector<Sample>> out(2, std::vector<Sample>(40));
    std::vector<std::span<Sample>>   outSpans{std::span<Sample>(out[0]), std::span<Sample>(out[1])};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 40) == 40);

    for (ChannelIndex ch = 0; ch < 2; ++ch) {
        for (std::size_t i = 0; i < 40; ++i) {
            REQUIRE(out[ch][i] == in[ch][i]);
        }
    }
    REQUIRE(ring.framesAvailableToRead() == 0);
}

TEST_CASE("RingBuffer write short-writes once full, read short-reads once empty", "[ring_buffer]") {
    RingBuffer ring(1, 16);  // capacity rounds to 16
    auto       in    = makePlanar(1, 20, 0.0f);
    auto       spans = constSpans(in);

    REQUIRE(ring.write(std::span<const std::span<const Sample>>(spans), 20) == 16);
    REQUIRE(ring.framesAvailableToWrite() == 0);

    std::vector<Sample>            out(20, -1.0f);
    std::vector<std::span<Sample>> outSpans{std::span<Sample>(out)};
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 20) == 16);
    REQUIRE(ring.read(std::span<std::span<Sample>>(outSpans), 4) == 0);
}

TEST_CASE("RingBuffer reset drops buffered content", "[ring_buffer]") {
    RingBuffer ring(1, 16);
    auto       in    = makePlanar(1, 8, 0.0f);
    auto       spans = constSpans(in);
    REQUIRE(ring.write(std::span<const std::span<const Sample>>(spans), 8) == 8);
    ring.reset();
    REQUIRE(ring.framesAvailableToRead() == 0);
    REQUIRE(ring.framesAvailableToWrite() == ring.capacityFrames());
}

TEST_CASE("RingBuffer survives randomised single-threaded producer/consumer interleavings", "[ring_buffer]") {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> writeSizeDist(0, 37);
    std::uniform_int_distribution<int> readSizeDist(0, 29);

    RingBuffer ring(1, 64);

    Sample                          nextToWrite = 0.0f;
    Sample                          nextToRead  = 0.0f;
    std::vector<Sample>             writeScratch(64);
    std::vector<Sample>             readScratch(64);

    for (int iter = 0; iter < 5000; ++iter) {
        const auto wantWrite = static_cast<std::size_t>(writeSizeDist(rng));
        for (std::size_t i = 0; i < wantWrite && i < writeScratch.size(); ++i) {
            writeScratch[i] = nextToWrite + static_cast<Sample>(i);
        }
        std::vector<std::span<const Sample>> writeSpans{std::span<const Sample>(writeScratch.data(), wantWrite)};
        const std::size_t written =
            ring.write(std::span<const std::span<const Sample>>(writeSpans), wantWrite);
        nextToWrite += static_cast<Sample>(written);

        const auto wantRead = static_cast<std::size_t>(readSizeDist(rng));
        std::vector<std::span<Sample>> readSpans{std::span<Sample>(readScratch.data(), wantRead)};
        const std::size_t read = ring.read(std::span<std::span<Sample>>(readSpans), wantRead);
        for (std::size_t i = 0; i < read; ++i) {
            REQUIRE(readScratch[i] == nextToRead + static_cast<Sample>(i));
        }
        nextToRead += static_cast<Sample>(read);
    }
}

TEST_CASE("RingBuffer is safe under real concurrent producer/consumer threads", "[ring_buffer]") {
    constexpr std::size_t kTotalFrames = 200000;
    RingBuffer             ring(1, 256);

    std::thread producer([&ring]() {
        std::mt19937                        rng(1);
        std::uniform_int_distribution<int>  chunkDist(1, 50);
        std::vector<Sample>                 scratch(50);
        std::size_t                         produced = 0;
        while (produced < kTotalFrames) {
            const auto want = std::min<std::size_t>(static_cast<std::size_t>(chunkDist(rng)), kTotalFrames - produced);
            for (std::size_t i = 0; i < want; ++i) {
                scratch[i] = static_cast<Sample>(produced + i);
            }
            std::vector<std::span<const Sample>> spans{std::span<const Sample>(scratch.data(), want)};
            std::size_t                          offset = 0;
            while (offset < want) {
                std::span<const Sample>              remaining(scratch.data() + offset, want - offset);
                std::vector<std::span<const Sample>> remSpans{remaining};
                const std::size_t written =
                    ring.write(std::span<const std::span<const Sample>>(remSpans), want - offset);
                offset += written;
                if (written == 0) {
                    std::this_thread::yield();
                }
            }
            produced += want;
        }
    });

    std::thread consumer([&ring]() {
        std::mt19937                        rng(2);
        std::uniform_int_distribution<int>  chunkDist(1, 40);
        std::vector<Sample>                 scratch(40);
        std::size_t                         consumed = 0;
        while (consumed < kTotalFrames) {
            const auto want = static_cast<std::size_t>(chunkDist(rng));
            std::vector<std::span<Sample>> spans{std::span<Sample>(scratch.data(), want)};
            const std::size_t got = ring.read(std::span<std::span<Sample>>(spans), want);
            for (std::size_t i = 0; i < got; ++i) {
                REQUIRE(scratch[i] == static_cast<Sample>(consumed + i));
            }
            consumed += got;
            if (got == 0) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
}
