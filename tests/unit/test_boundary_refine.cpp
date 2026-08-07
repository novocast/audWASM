#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <span>
#include <vector>

#include "../../engine/analysis/silence/boundary_refine.hpp"

using Catch::Approx;
using aud::AudioBuffer;
using aud::FrameRange;
using aud::Sample;
using aud::silence::ChannelMode;
using aud::silence::nearestZeroCrossing;
using aud::silence::refineRegionBoundaries;
using aud::silence::SilenceKind;
using aud::silence::SilencePosition;
using aud::silence::SilenceRegion;

namespace {

constexpr aud::SampleRate kSampleRate = 48000;

AudioBuffer makeBuffer(const std::vector<Sample>& mono) {
    auto result = AudioBuffer::create(kSampleRate, 1);
    REQUIRE(result.has_value());
    auto buffer = std::move(result).value();
    std::vector<std::span<const Sample>> planar{mono};
    REQUIRE(buffer.append(planar, mono.size()).has_value());
    return buffer;
}

}  // namespace

TEST_CASE("boundary_refine: sample-precise start/end within ±1ms of ground truth", "[silence][boundary_refine]") {
    // Loud (0.5) for 1.0s, exact digital silence for 2.0s, loud again for 1.0s. Ground truth
    // boundaries are at frame 48000 and frame 144000.
    constexpr std::size_t loudFrames   = kSampleRate;       // 1.0s
    constexpr std::size_t silentFrames = kSampleRate * 2;   // 2.0s
    std::vector<Sample> mono;
    mono.insert(mono.end(), loudFrames, 0.5f);
    mono.insert(mono.end(), silentFrames, 0.0f);
    mono.insert(mono.end(), loudFrames, 0.5f);

    auto buffer = makeBuffer(mono);

    // Coarse region deliberately off by up to a window's width in both directions, as a window-
    // grid pass could produce.
    SilenceRegion region;
    region.kind     = SilenceKind::Digital;
    region.position = SilencePosition::Internal;
    region.range    = FrameRange{static_cast<aud::FrameIndex>(loudFrames) - 2000, static_cast<aud::FrameIndex>(loudFrames + silentFrames) + 2000};

    std::vector<SilenceRegion> regions{region};
    constexpr std::size_t windowFrames = 2400;  // 50ms @ 48kHz
    REQUIRE(refineRegionBoundaries(buffer, regions, windowFrames, /*thresholdLinear=*/0.0, ChannelMode::All).has_value());

    REQUIRE(regions.size() == 1);
    const double startErrorMs = std::abs(regions[0].startSeconds - 1.0) * 1000.0;
    const double endErrorMs   = std::abs(regions[0].endSeconds - 3.0) * 1000.0;
    CHECK(startErrorMs <= 1.0);
    CHECK(endErrorMs <= 1.0);
}

TEST_CASE("boundary_refine: recomputes exact peak/rms within the refined region", "[silence][boundary_refine]") {
    std::vector<Sample> mono(kSampleRate, 0.25f);  // 1.0s at a constant 0.25 amplitude, all "silent"
    auto buffer = makeBuffer(mono);

    SilenceRegion region;
    region.kind     = SilenceKind::Threshold;
    region.position = SilencePosition::EntireFile;
    region.range    = FrameRange{0, static_cast<aud::FrameIndex>(mono.size())};

    std::vector<SilenceRegion> regions{region};
    // thresholdLinear above 0.25 so the whole buffer reads as silent and boundaries don't move.
    REQUIRE(refineRegionBoundaries(buffer, regions, 2400, /*thresholdLinear=*/0.3, ChannelMode::All).has_value());

    REQUIRE(regions.size() == 1);
    const double expectedDb = 20.0 * std::log10(0.25);
    CHECK(regions[0].rmsDbfsWithin == Approx(expectedDb).margin(1e-3));
    CHECK(regions[0].peakDbfsWithin == Approx(expectedDb).margin(1e-3));
}

TEST_CASE("boundary_refine: perceptual regions are left untouched", "[silence][boundary_refine]") {
    std::vector<Sample> mono(4800, 0.0f);
    auto buffer = makeBuffer(mono);

    SilenceRegion region;
    region.kind          = SilenceKind::Perceptual;
    region.position      = SilencePosition::EntireFile;
    region.range          = FrameRange{100, 4700};
    region.rmsDbfsWithin  = -75.0;

    std::vector<SilenceRegion> regions{region};
    REQUIRE(refineRegionBoundaries(buffer, regions, 2400, 0.0, ChannelMode::All).has_value());

    CHECK(regions[0].range.begin == 100);
    CHECK(regions[0].range.end == 4700);
    CHECK(regions[0].rmsDbfsWithin == Approx(-75.0));
}

TEST_CASE("boundary_refine: nearestZeroCrossing finds the crossing nearest the given frame", "[silence][boundary_refine]") {
    // A signal that is negative for frames [0,50), exactly zero at 50, positive after.
    std::vector<Sample> mono(200, 0.0f);
    for (std::size_t i = 0; i < 50; ++i) mono[i] = -0.5f;
    for (std::size_t i = 51; i < 200; ++i) mono[i] = 0.5f;

    auto buffer = makeBuffer(mono);
    const auto crossing = nearestZeroCrossing(buffer, 0, /*frame=*/60, /*searchRadius=*/30);
    CHECK(crossing == 50);
}

TEST_CASE("boundary_refine: nearestZeroCrossing returns the original frame when none is in range", "[silence][boundary_refine]") {
    std::vector<Sample> mono(200, 0.3f);  // no crossing anywhere
    auto buffer = makeBuffer(mono);
    const auto crossing = nearestZeroCrossing(buffer, 0, /*frame=*/100, /*searchRadius=*/10);
    CHECK(crossing == 100);
}
