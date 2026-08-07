#include "boundary_refine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aud::silence {

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double linearToDbfs(double linear) {
    return linear <= 0.0 ? kNegInf : 20.0 * std::log10(linear);
}

// Reads [lo, hi) for every channel into `out[c]`. Returns false (leaving `out` untouched) on any
// read failure so callers can fall back to the coarse boundary rather than half-apply garbage.
bool readAllChannels(const AudioBuffer& buffer, FrameIndex lo, FrameIndex hi, std::vector<std::vector<Sample>>& out) {
    const auto channels = buffer.channelCount();
    out.assign(channels, std::vector<Sample>(static_cast<std::size_t>(hi - lo)));
    for (ChannelIndex c = 0; c < channels; ++c) {
        auto res = buffer.read(c, FrameRange{lo, hi}, out[c]);
        if (!res.has_value()) return false;
    }
    return true;
}

// Whether frame `i` (index into the per-channel buffers read by readAllChannels) is "silent" under
// the same channelMode combination rule as the coarse pass (M10: "All: silent only if every
// channel is silent").
bool frameIsSilent(const std::vector<std::vector<Sample>>& perChannel, std::size_t i, double thresholdLinear,
                    ChannelMode mode) {
    std::size_t silentCount = 0;
    for (const auto& channel : perChannel) {
        if (std::abs(static_cast<double>(channel[i])) <= thresholdLinear) ++silentCount;
    }
    return mode == ChannelMode::All ? silentCount == perChannel.size() : silentCount > 0;
}

// Refines one boundary. `coarse` is the window-grid frame; the true boundary is searched within
// ±windowFrames of it. `wantsEntry` selects which edge this is: true for a region's start (the
// transition loud -> silent, so we want the last loud sample's successor), false for a region's
// end (the transition silent -> loud, so we want the first loud sample after it).
FrameIndex refineOneBoundary(const AudioBuffer& buffer, FrameIndex coarse, std::size_t windowFrames,
                              double thresholdLinear, ChannelMode channelMode, bool wantsEntry) {
    const FrameIndex total = buffer.frameCount();
    if (total <= 0) return coarse;

    const FrameIndex lo = std::max<FrameIndex>(0, coarse - static_cast<FrameIndex>(windowFrames));
    const FrameIndex hi = std::min<FrameIndex>(total, coarse + static_cast<FrameIndex>(windowFrames));
    if (hi <= lo) return coarse;

    std::vector<std::vector<Sample>> perChannel;
    if (!readAllChannels(buffer, lo, hi, perChannel)) return coarse;

    const std::size_t n = static_cast<std::size_t>(hi - lo);

    if (wantsEntry) {
        // Scan forward; refined start = index right after the last loud sample in range. If the
        // whole search window is silent, the true boundary is further back than ±1 window away —
        // keep the coarse estimate rather than guess.
        std::size_t lastLoud = n;  // sentinel: "not found"
        for (std::size_t i = 0; i < n; ++i) {
            if (!frameIsSilent(perChannel, i, thresholdLinear, channelMode)) lastLoud = i;
        }
        if (lastLoud == n) return coarse;
        return lo + static_cast<FrameIndex>(lastLoud + 1);
    } else {
        // Scan forward; refined end = index of the first loud sample in range. If none is found,
        // the region continues past this window — keep the coarse estimate.
        for (std::size_t i = 0; i < n; ++i) {
            if (!frameIsSilent(perChannel, i, thresholdLinear, channelMode)) return lo + static_cast<FrameIndex>(i);
        }
        return coarse;
    }
}

}  // namespace

Result<void> refineRegionBoundaries(const AudioBuffer& buffer, std::vector<SilenceRegion>& regions,
                                     std::size_t windowFrames, double thresholdLinear, ChannelMode channelMode) {
    if (windowFrames == 0) {
        return Error{ErrorCode::InvalidArgument, "analysis.silence", "refineRegionBoundaries requires windowFrames > 0"};
    }

    const SampleRate sampleRate = buffer.sampleRate();

    for (SilenceRegion& region : regions) {
        if (region.kind == SilenceKind::Perceptual) continue;  // no per-sample test in the loudness domain

        FrameIndex refinedBegin = region.range.begin;
        FrameIndex refinedEnd   = region.range.end;

        if (region.position != SilencePosition::Leading && region.position != SilencePosition::EntireFile) {
            refinedBegin = refineOneBoundary(buffer, region.range.begin, windowFrames, thresholdLinear, channelMode,
                                              /*wantsEntry=*/true);
        }
        if (region.position != SilencePosition::Trailing && region.position != SilencePosition::EntireFile) {
            refinedEnd = refineOneBoundary(buffer, region.range.end, windowFrames, thresholdLinear, channelMode,
                                            /*wantsEntry=*/false);
        }
        if (refinedEnd <= refinedBegin) {
            // Guard against pathological refinement collapsing the region (e.g. a click right at
            // the coarse boundary on both sides); fall back to the coarse range unchanged.
            refinedBegin = region.range.begin;
            refinedEnd   = region.range.end;
        }

        region.range        = FrameRange{refinedBegin, refinedEnd};
        region.startSeconds = sampleRate == 0 ? 0.0 : static_cast<double>(refinedBegin) / static_cast<double>(sampleRate);
        region.endSeconds   = sampleRate == 0 ? 0.0 : static_cast<double>(refinedEnd) / static_cast<double>(sampleRate);

        // Recompute level stats from the actual samples in the refined region — exact, unlike the
        // coarse pass's window-average approximation. Cost is proportional to this region's
        // duration, not the file's.
        std::vector<std::vector<Sample>> perChannel;
        if (readAllChannels(buffer, refinedBegin, refinedEnd, perChannel) && !perChannel.empty()) {
            double sumSquares = 0.0;
            double peak        = 0.0;
            std::uint64_t count = 0;
            for (const auto& channel : perChannel) {
                for (Sample s : channel) {
                    const double v = static_cast<double>(s);
                    sumSquares += v * v;
                    peak = std::max(peak, std::abs(v));
                    ++count;
                }
            }
            region.rmsDbfsWithin  = count == 0 ? kNegInf : linearToDbfs(std::sqrt(sumSquares / static_cast<double>(count)));
            region.peakDbfsWithin = linearToDbfs(peak);
        }
    }

    return {};
}

FrameIndex nearestZeroCrossing(const AudioBuffer& buffer, ChannelIndex ch, FrameIndex frame, FrameIndex searchRadius) {
    if (ch >= buffer.channelCount() || frame < 0 || buffer.frameCount() <= 0 || searchRadius < 0) return frame;

    const FrameIndex total = buffer.frameCount();
    const FrameIndex lo    = std::max<FrameIndex>(0, frame - searchRadius);
    const FrameIndex hi    = std::min<FrameIndex>(total, frame + searchRadius + 1);
    if (hi <= lo) return frame;

    std::vector<Sample> buf(static_cast<std::size_t>(hi - lo));
    if (!buffer.read(ch, FrameRange{lo, hi}, buf).has_value()) return frame;

    const std::size_t n      = buf.size();
    const std::size_t center = static_cast<std::size_t>(frame - lo);

    // Search outward from `frame` by increasing radius so the closest crossing wins.
    for (std::size_t d = 0; d <= n; ++d) {
        for (long dir = -1; dir <= 1; dir += 2) {
            if (d == 0 && dir > 0) continue;  // radius-0 only once
            const long idx = static_cast<long>(center) + dir * static_cast<long>(d);
            if (idx < 0 || static_cast<std::size_t>(idx) >= n) continue;
            const std::size_t i = static_cast<std::size_t>(idx);

            if (buf[i] == 0.0f) return lo + static_cast<FrameIndex>(i);
            if (i + 1 < n && buf[i] != 0.0f && buf[i + 1] != 0.0f && (buf[i] < 0.0f) != (buf[i + 1] < 0.0f)) {
                // Sign change between i and i+1: report whichever sample is closer to zero.
                return lo + static_cast<FrameIndex>(std::abs(buf[i]) <= std::abs(buf[i + 1]) ? i : i + 1);
            }
        }
    }
    return frame;
}

}  // namespace aud::silence
