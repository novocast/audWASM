#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "../../engine/analysis/beats/peak_pick.hpp"

using Catch::Approx;
using aud::beats::pickPeaks;
using aud::beats::PeakPickConfig;

TEST_CASE("beats::pickPeaks: isolated Gaussian bumps are each found once, sub-frame refined toward the true peak",
          "[beats][peak_pick]") {
    constexpr std::size_t n           = 300;
    constexpr double      hopSeconds  = 512.0 / 44100.0;
    const std::vector<double> trueCenters = {40.3, 140.7, 241.0};

    std::vector<float> odf(n, 0.0f);
    for (double center : trueCenters) {
        for (std::size_t i = 0; i < n; ++i) {
            const double d = static_cast<double>(i) - center;
            odf[i] += static_cast<float>(std::exp(-0.5 * d * d / (1.2 * 1.2)));
        }
    }

    PeakPickConfig config;
    config.delta = 0.05f;
    const auto peaks = pickPeaks(odf, hopSeconds, config);

    REQUIRE(peaks.size() == trueCenters.size());
    for (std::size_t i = 0; i < peaks.size(); ++i) {
        const double refined = static_cast<double>(peaks[i].frameIndex) + peaks[i].subFrameOffset;
        CHECK(refined == Approx(trueCenters[i]).margin(0.3));
    }
}

TEST_CASE("beats::pickPeaks: the refractory period suppresses a second peak too close to the first",
          "[beats][peak_pick]") {
    std::vector<float> odf(100, 0.0f);
    odf[50] = 1.0f;
    odf[53] = 1.0f;  // both are legitimate local maxima, but only 3 frames (~35ms) apart

    PeakPickConfig config;
    config.minInterOnsetSeconds = 0.05;  // ~4.3 frames at this hop — wider than the 3-frame gap above
    const auto peaks = pickPeaks(odf, 512.0 / 44100.0, config);

    REQUIRE(peaks.size() == 1);
    CHECK(peaks[0].frameIndex == 50);
}

TEST_CASE("beats::pickPeaks: flat/silent ODF produces no peaks", "[beats][peak_pick]") {
    std::vector<float> odf(200, 0.0f);
    const auto         peaks = pickPeaks(odf, 512.0 / 44100.0, PeakPickConfig{});
    CHECK(peaks.empty());
}
