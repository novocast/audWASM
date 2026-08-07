#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <span>
#include <vector>

#include "../../engine/analysis/clipping/limiting_heuristic.hpp"

using Catch::Approx;
using aud::Sample;
using aud::clipping::LimitingHeuristicAccumulator;
using aud::clipping::LimitingHeuristicResult;

namespace {
constexpr double kPi = 3.14159265358979323846;

LimitingHeuristicResult runMono(const std::vector<Sample>& samples, double peakLinear) {
    LimitingHeuristicAccumulator acc;
    acc.begin(1);
    acc.process(0, std::span<const Sample>(samples));
    std::vector<double> peaks{peakLinear};
    return acc.finish(peaks);
}
}  // namespace

TEST_CASE("limiting_heuristic: a signal sitting at its peak for most of its length reads heavy "
          "limiting",
          "[clipping]") {
    // A brick-wall-limited-looking signal: mostly pinned at 0.99 (within 0.5 dB of the 0.99 peak),
    // with only brief dips.
    std::vector<Sample> samples(10000, 0.99f);
    for (std::size_t i = 0; i < samples.size(); i += 50) samples[i] = 0.3f;  // occasional dip

    auto result = runMono(samples, 0.99);

    CHECK(result.flatTopRatio > 0.9);
    CHECK(result.heavyLimitingLikely);
}

TEST_CASE("limiting_heuristic: an ordinary sine reads low flat-top ratio, no heavy limiting", "[clipping]") {
    constexpr std::size_t period = 480;
    std::vector<Sample>   sine(period * 200);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<Sample>(0.9 * std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(period)));
    }

    auto result = runMono(sine, 0.9);

    // A pure sine's own curvature already puts a non-trivial slice of every cycle within 0.5 dB of
    // its peak (analytically ~21% — see limiting_heuristic.hpp's threshold comment), but nowhere
    // near a genuinely brick-walled signal's ratio (the previous test measures ~0.98).
    CHECK(result.flatTopRatio < 0.3);
    CHECK_FALSE(result.heavyLimitingLikely);
}

TEST_CASE("limiting_heuristic: mean plateau length matches constructed runs of identical samples",
          "[clipping]") {
    // Five isolated runs of 4 bit-identical samples each, separated by non-repeating values.
    std::vector<Sample> samples;
    for (int r = 0; r < 5; ++r) {
        for (int j = 0; j < 4; ++j) samples.push_back(0.5f);
        samples.push_back(0.1f);
        samples.push_back(0.2f);
        samples.push_back(0.3f);
    }

    auto result = runMono(samples, 0.5);

    CHECK(result.meanPlateauLength == Approx(4.0).margin(1e-9));
}

TEST_CASE("limiting_heuristic: a silent channel reports zero flat-top ratio, not a divide-by-zero "
          "NaN",
          "[clipping]") {
    std::vector<Sample> silence(1000, 0.0f);
    auto                result = runMono(silence, 0.0);

    CHECK(result.flatTopRatio == Approx(0.0).margin(1e-12));
    CHECK_FALSE(result.heavyLimitingLikely);
}
