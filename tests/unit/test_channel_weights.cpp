#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../../engine/analysis/loudness/channel_weights.hpp"

using Catch::Approx;
using aud::loudness::resolveChannelRolesByCount;
using aud::loudness::resolveChannelRolesFromWavMask;
using aud::loudness::WavSpeakerBit;

TEST_CASE("channel_weights: mono and stereo weight every channel 1.0", "[loudness]") {
    auto mono = resolveChannelRolesByCount(1);
    REQUIRE(mono.weights.size() == 1);
    CHECK(mono.weights[0] == Approx(1.0));

    auto stereo = resolveChannelRolesByCount(2);
    REQUIRE(stereo.weights.size() == 2);
    CHECK(stereo.weights[0] == Approx(1.0));
    CHECK(stereo.weights[1] == Approx(1.0));
}

TEST_CASE("channel_weights: 5.0 (L R C Ls Rs) weights surrounds 1.41 with no LFE to exclude", "[loudness]") {
    auto surround = resolveChannelRolesByCount(5);
    REQUIRE(surround.weights.size() == 5);
    CHECK(surround.weights[0] == Approx(1.0));   // L
    CHECK(surround.weights[1] == Approx(1.0));   // R
    CHECK(surround.weights[2] == Approx(1.0));   // C
    CHECK(surround.weights[3] == Approx(1.41));  // Ls
    CHECK(surround.weights[4] == Approx(1.41));  // Rs
}

TEST_CASE("channel_weights: 5.1 (L R C LFE Ls Rs) excludes LFE and weights surrounds 1.41", "[loudness]") {
    auto surround = resolveChannelRolesByCount(6);
    REQUIRE(surround.weights.size() == 6);
    CHECK(surround.weights[0] == Approx(1.0));   // L
    CHECK(surround.weights[1] == Approx(1.0));   // R
    CHECK(surround.weights[2] == Approx(1.0));   // C
    CHECK(surround.weights[3] == Approx(0.0));   // LFE — excluded entirely
    CHECK(surround.weights[4] == Approx(1.41));  // Ls
    CHECK(surround.weights[5] == Approx(1.41));  // Rs
}

TEST_CASE("channel_weights: unknown channel counts fall back to 1.0 and flag the fallback", "[loudness]") {
    auto quad = resolveChannelRolesByCount(4);
    REQUIRE(quad.weights.size() == 4);
    for (double w : quad.weights) CHECK(w == Approx(1.0));
    CHECK(quad.usedFallback);
}

TEST_CASE("channel_weights: WAVE_FORMAT_EXTENSIBLE mask resolves the same 5.1 weighting", "[loudness]") {
    const std::uint32_t mask = static_cast<std::uint32_t>(WavSpeakerBit::FrontLeft) |
                                static_cast<std::uint32_t>(WavSpeakerBit::FrontRight) |
                                static_cast<std::uint32_t>(WavSpeakerBit::FrontCenter) |
                                static_cast<std::uint32_t>(WavSpeakerBit::LowFrequency) |
                                static_cast<std::uint32_t>(WavSpeakerBit::BackLeft) |
                                static_cast<std::uint32_t>(WavSpeakerBit::BackRight);

    auto resolved = resolveChannelRolesFromWavMask(6, mask);
    REQUIRE(resolved.weights.size() == 6);
    CHECK_FALSE(resolved.usedFallback);
    CHECK(resolved.weights[0] == Approx(1.0));   // FL
    CHECK(resolved.weights[1] == Approx(1.0));   // FR
    CHECK(resolved.weights[2] == Approx(1.0));   // FC
    CHECK(resolved.weights[3] == Approx(0.0));   // LFE
    CHECK(resolved.weights[4] == Approx(1.41));  // BL
    CHECK(resolved.weights[5] == Approx(1.41));  // BR
}

TEST_CASE("channel_weights: side-left/side-right mask bits also get surround weighting", "[loudness]") {
    const std::uint32_t mask = static_cast<std::uint32_t>(WavSpeakerBit::FrontLeft) |
                                static_cast<std::uint32_t>(WavSpeakerBit::FrontRight) |
                                static_cast<std::uint32_t>(WavSpeakerBit::SideLeft) |
                                static_cast<std::uint32_t>(WavSpeakerBit::SideRight);

    auto resolved = resolveChannelRolesFromWavMask(4, mask);
    REQUIRE(resolved.weights.size() == 4);
    CHECK(resolved.weights[0] == Approx(1.0));
    CHECK(resolved.weights[1] == Approx(1.0));
    CHECK(resolved.weights[2] == Approx(1.41));
    CHECK(resolved.weights[3] == Approx(1.41));
}
