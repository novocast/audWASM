#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <vector>

#include "../../engine/util/accumulate.hpp"

using Catch::Approx;
using aud::pairwiseSum;
using aud::pairwiseSumSquares;
using aud::Sample;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Deliberately the naive implementation this codebase's numerical-care rule exists to avoid: a
// running accumulator kept in `float` (not `double`), added to left-to-right. M09/M04's actual
// accumulators always promote to double before summing (pairwiseSum<double>'s signature does this
// per term) — this is what an unwary implementation would do instead.
float naiveSumSquaresFloat(const std::vector<Sample>& samples) {
    float sum = 0.0f;
    for (Sample s : samples) sum += s * s;
    return sum;
}
}  // namespace

// M09 task list: "assert the naive version *would* be wrong, so the test proves the mitigation
// matters". Sum-of-squares (the RMS/variance workhorse) is where this bites: a 10-minute -80 dBFS
// tone at 44.1kHz is ~26.46M samples, and the running sum-of-squares grows to a magnitude (~0.13)
// where a single-precision accumulator's step size becomes comparable to (or larger than) each
// per-sample contribution (~5e-9) — additions get silently absorbed (rounded away entirely) for
// long stretches once that happens, which pairwise summation avoids by restructuring the
// reduction into a balanced tree of same-magnitude partial sums instead of one ever-growing
// accumulator.
TEST_CASE("pairwise summation beats naive float accumulation on a long, quiet tone", "[statistics][accumulate]") {
    constexpr aud::SampleRate sampleRate    = 44100;
    constexpr double          amplitude     = 1e-4;  // -80 dBFS
    constexpr std::size_t     durationSecs  = 600;
    const std::size_t         n             = static_cast<std::size_t>(sampleRate) * durationSecs;
    constexpr std::size_t     periodSamples = 100;

    std::vector<Sample> samples(n);
    for (std::size_t i = 0; i < n; ++i) {
        samples[i] =
            static_cast<Sample>(amplitude * std::sin(2.0 * kPi * static_cast<double>(i) / static_cast<double>(periodSamples)));
    }
    REQUIRE(n % periodSamples == 0);  // whole number of periods, so the analytic answer below is exact

    // Analytic mean-square of a full-period sine of this amplitude, times n.
    const double expectedSumSquares = static_cast<double>(n) * amplitude * amplitude / 2.0;

    const double naiveError    = std::fabs(static_cast<double>(naiveSumSquaresFloat(samples)) - expectedSumSquares);
    const double pairwiseError = std::fabs(pairwiseSumSquares<double>(samples) - expectedSumSquares);

    // Pairwise (promoting to double, reducing as a balanced tree) stays close to exact; naive
    // single-precision accumulation drifts measurably — the whole point of the mitigation.
    CHECK(pairwiseError < expectedSumSquares * 1e-6);
    CHECK(naiveError > pairwiseError * 10.0);
}

TEST_CASE("pairwise summation matches a plain double sum on a small array", "[statistics][accumulate]") {
    std::vector<Sample> samples{0.1f, -0.2f, 0.3f, -0.4f, 0.5f};

    double naiveSum = 0.0;
    for (Sample s : samples) naiveSum += static_cast<double>(s);
    double naiveSumSq = 0.0;
    for (Sample s : samples) naiveSumSq += static_cast<double>(s) * static_cast<double>(s);

    CHECK(pairwiseSum<double>(samples) == Approx(naiveSum));
    CHECK(pairwiseSumSquares<double>(samples) == Approx(naiveSumSq));
}
