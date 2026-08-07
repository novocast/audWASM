#include "silence_detector.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>

namespace aud::silence {

namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

double linearToDbfs(double linear) {
    return linear <= 0.0 ? kNegInf : 20.0 * std::log10(linear);
}

std::string jsonNumber(double v) {
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e999" : "-1e999";  // JSON has no Infinity; sentinel large magnitude
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

const char* kindName(SilenceKind kind) {
    switch (kind) {
        case SilenceKind::Digital:    return "digital";
        case SilenceKind::Threshold:  return "threshold";
        case SilenceKind::Perceptual: return "perceptual";
    }
    return "unknown";
}

const char* positionName(SilencePosition position) {
    switch (position) {
        case SilencePosition::Leading:    return "leading";
        case SilencePosition::Internal:   return "internal";
        case SilencePosition::Trailing:   return "trailing";
        case SilencePosition::EntireFile: return "entireFile";
    }
    return "unknown";
}

// One RLE run of silent windows, in window-grid coordinates, half-open [begin, end).
struct Run {
    std::size_t   begin = 0;
    std::size_t   end   = 0;
    std::uint32_t channelMask = 0;  // OR of every window's contributing-channel mask in this run
};

// Per-window, per-channel booleans -> combined-per-window booleans + channel mask, honouring
// channelMode (M10: "All: silent only if every channel is silent").
void combineChannels(const std::vector<std::uint8_t>& silentPerChannel, std::size_t windowCount,
                      std::uint32_t channelCount, ChannelMode mode, std::vector<bool>& windowSilentOut,
                      std::vector<std::uint32_t>& channelMaskOut) {
    windowSilentOut.assign(windowCount, false);
    channelMaskOut.assign(windowCount, 0);
    for (std::size_t w = 0; w < windowCount; ++w) {
        std::uint32_t mask = 0;
        std::uint32_t silentCount = 0;
        for (std::uint32_t c = 0; c < channelCount; ++c) {
            if (silentPerChannel[w * channelCount + c]) {
                mask |= (1u << c);
                ++silentCount;
            }
        }
        channelMaskOut[w] = mask;
        windowSilentOut[w] = (mode == ChannelMode::All) ? (silentCount == channelCount) : (silentCount > 0);
    }
}

// Hysteresis Schmitt trigger per channel: enter silence once level <= enterDb, exit only once
// level > exitDb (M10: "Enter silence below thresholdDb, exit only above thresholdDb + hysteresisDb").
// Channel starts in the "silent" state so leading silence is detected from window 0.
void classifyThresholdChannel(const std::vector<float>& rmsSeries, std::size_t windowCount, std::uint32_t channelCount,
                               std::uint32_t channel, double enterDb, double exitDb,
                               std::vector<std::uint8_t>& silentOut /* windowCount*channelCount, indexed [w*channelCount+c] */) {
    bool silent = true;
    for (std::size_t w = 0; w < windowCount; ++w) {
        const double level = linearToDbfs(static_cast<double>(rmsSeries[w * channelCount + channel]));
        if (silent) {
            if (level > exitDb) silent = false;
        } else {
            if (level <= enterDb) silent = true;
        }
        silentOut[w * channelCount + channel] = silent ? 1 : 0;
    }
}

std::vector<Run> runLengthEncode(const std::vector<bool>& windowSilent, const std::vector<std::uint32_t>& channelMask) {
    std::vector<Run> runs;
    std::size_t w = 0;
    const std::size_t n = windowSilent.size();
    while (w < n) {
        if (!windowSilent[w]) { ++w; continue; }
        Run run;
        run.begin = w;
        std::uint32_t mask = 0;
        while (w < n && windowSilent[w]) {
            mask |= channelMask[w];
            ++w;
        }
        run.end = w;
        run.channelMask = mask;
        runs.push_back(run);
    }
    return runs;
}

// Bridge runs separated by a non-silent gap shorter than mergeGapMs (M10: "a single loud sample in
// the middle of a 30-second silent passage should not split it into two regions").
void mergeRuns(std::vector<Run>& runs, double windowSeconds, double mergeGapMs) {
    if (runs.empty()) return;
    const double gapWindows = (mergeGapMs / 1000.0) / windowSeconds;
    std::vector<Run> merged;
    merged.push_back(runs.front());
    for (std::size_t i = 1; i < runs.size(); ++i) {
        Run& last = merged.back();
        const double gap = static_cast<double>(runs[i].begin - last.end);
        if (gap < gapWindows) {
            last.end = runs[i].end;
            last.channelMask |= runs[i].channelMask;
        } else {
            merged.push_back(runs[i]);
        }
    }
    runs = std::move(merged);
}

void discardShortRuns(std::vector<Run>& runs, double windowSeconds, double minDurationMs) {
    const double minWindows = (minDurationMs / 1000.0) / windowSeconds;
    runs.erase(std::remove_if(runs.begin(), runs.end(),
                               [minWindows](const Run& r) {
                                   return static_cast<double>(r.end - r.begin) < minWindows;
                               }),
               runs.end());
}

SilenceResult emptyResult(const SilenceParameters& params) {
    SilenceResult result;
    result.parametersUsed = params;
    return result;
}

SilencePosition classifyPosition(const Run& run, std::size_t windowCount) {
    const bool touchesStart = run.begin == 0;
    const bool touchesEnd   = run.end == windowCount;
    if (touchesStart && touchesEnd) return SilencePosition::EntireFile;
    if (touchesStart) return SilencePosition::Leading;
    if (touchesEnd) return SilencePosition::Trailing;
    return SilencePosition::Internal;
}

// Builds the final SilenceResult from merged/filtered runs. `levelForStats(w)` returns a value
// used for the region's reported level: linear RMS for threshold/digital (converted to dBFS here),
// already-in-dB LUFS for perceptual (passed through as-is via `statsAreAlreadyDb`).
SilenceResult buildResult(std::vector<Run> runs, std::size_t windowCount, double windowSeconds, SampleRate sampleRate,
                           FrameIndex frameCount, SilenceKind kind, const SilenceParameters& params,
                           const std::function<double(std::size_t)>& levelForStats, bool statsAreAlreadyDb) {
    SilenceResult result;
    result.parametersUsed = params;

    const std::size_t windowFrames =
        sampleRate == 0 ? 0 : static_cast<std::size_t>(windowSeconds * static_cast<double>(sampleRate) + 0.5);

    for (const Run& run : runs) {
        SilenceRegion region;
        region.kind        = kind;
        region.position    = classifyPosition(run, windowCount);
        region.channelMask = run.channelMask;

        const FrameIndex startFrame = static_cast<FrameIndex>(run.begin * windowFrames);
        FrameIndex endFrame         = static_cast<FrameIndex>(run.end * windowFrames);
        if (run.end == windowCount && frameCount != kNoFrame) endFrame = frameCount;  // last window may be partial
        region.range        = FrameRange{startFrame, endFrame};
        region.startSeconds = sampleRate == 0 ? 0.0 : static_cast<double>(startFrame) / static_cast<double>(sampleRate);
        region.endSeconds   = sampleRate == 0 ? 0.0 : static_cast<double>(endFrame) / static_cast<double>(sampleRate);

        double sum = 0.0;
        double maxLevel = kNegInf;
        for (std::size_t w = run.begin; w < run.end; ++w) {
            const double v = levelForStats(w);
            sum += v;
            if (v > maxLevel) maxLevel = v;
        }
        const double count = static_cast<double>(run.end - run.begin);
        const double meanLevel = count > 0.0 ? sum / count : kNegInf;

        if (statsAreAlreadyDb) {
            region.rmsDbfsWithin  = meanLevel;  // LUFS, see header comment
            region.peakDbfsWithin = kNegInf;    // not meaningful in the loudness domain
        } else {
            region.rmsDbfsWithin  = linearToDbfs(meanLevel);
            region.peakDbfsWithin = linearToDbfs(maxLevel);  // coarse: loudest *window*, not sample peak
        }

        result.regions.push_back(region);

        const double duration = region.endSeconds - region.startSeconds;
        result.totalSilenceSeconds += duration;
        if (region.position == SilencePosition::Leading || region.position == SilencePosition::EntireFile) {
            result.leadingSilenceSeconds = std::max(result.leadingSilenceSeconds, duration);
        }
        if (region.position == SilencePosition::Trailing || region.position == SilencePosition::EntireFile) {
            result.trailingSilenceSeconds = std::max(result.trailingSilenceSeconds, duration);
        }
    }

    const double totalSeconds = sampleRate == 0 || frameCount <= 0
                                     ? 0.0
                                     : static_cast<double>(frameCount) / static_cast<double>(sampleRate);
    result.silenceFraction = totalSeconds > 0.0 ? std::min(1.0, result.totalSilenceSeconds / totalSeconds) : 0.0;

    return result;
}

}  // namespace

SilenceResult SilenceDetector::detectThreshold(const SilenceInput& input, const SilenceParameters& params) {
    const std::size_t   windowCount   = input.rmsWindowCount();
    const std::uint32_t channelCount  = input.channelCount;
    if (windowCount == 0 || channelCount == 0) return emptyResult(params);

    const double enterDb = params.thresholdDb;
    const double exitDb  = params.useHysteresis ? params.thresholdDb + params.hysteresisDb : params.thresholdDb;

    std::vector<std::uint8_t> silentPerChannel(windowCount * channelCount, 0);
    for (std::uint32_t c = 0; c < channelCount; ++c) {
        classifyThresholdChannel(input.rmsSeries, windowCount, channelCount, c, enterDb, exitDb, silentPerChannel);
    }

    std::vector<bool>         windowSilent;
    std::vector<std::uint32_t> channelMask;
    combineChannels(silentPerChannel, windowCount, channelCount, params.channelMode, windowSilent, channelMask);

    auto runs = runLengthEncode(windowSilent, channelMask);
    mergeRuns(runs, input.rmsWindowSeconds, params.mergeGapMs);
    discardShortRuns(runs, input.rmsWindowSeconds, params.minDurationMs);

    // Region-level stat: RMS across whichever channels the region cares about (mean of per-channel
    // linear RMS for that window — cheap, coarse; refineRegionLevels() replaces this with an exact
    // sample-domain figure once PCM is available).
    auto levelForStats = [&](std::size_t w) {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < channelCount; ++c) sum += static_cast<double>(input.rmsSeries[w * channelCount + c]);
        return sum / static_cast<double>(channelCount);
    };

    return buildResult(std::move(runs), windowCount, input.rmsWindowSeconds, input.sampleRate, input.frameCount,
                        SilenceKind::Threshold, params, levelForStats, /*statsAreAlreadyDb=*/false);
}

SilenceResult SilenceDetector::detectDigital(const SilenceInput& input, const SilenceParameters& params) {
    const std::size_t   windowCount  = input.rmsWindowCount();
    const std::uint32_t channelCount = input.channelCount;
    if (windowCount == 0 || channelCount == 0 || input.digitalSilenceSeries.size() != windowCount * channelCount) {
        return emptyResult(params);
    }

    // Exact boolean test, no hysteresis: a window either was all-zero or it wasn't.
    std::vector<bool>          windowSilent;
    std::vector<std::uint32_t> channelMask;
    combineChannels(input.digitalSilenceSeries, windowCount, channelCount, params.channelMode, windowSilent, channelMask);

    auto runs = runLengthEncode(windowSilent, channelMask);
    mergeRuns(runs, input.rmsWindowSeconds, params.mergeGapMs);
    discardShortRuns(runs, input.rmsWindowSeconds, params.minDurationMs);

    // Digital silence is by definition -inf; still route through the RMS series (mirrors
    // detectThreshold) so a region that's mostly-but-not-entirely exact-zero reports something
    // sane if that ever happens (e.g. after merging across a single non-zero sample).
    auto levelForStats = [&](std::size_t w) {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < channelCount; ++c) sum += static_cast<double>(input.rmsSeries[w * channelCount + c]);
        return sum / static_cast<double>(channelCount);
    };

    return buildResult(std::move(runs), windowCount, input.rmsWindowSeconds, input.sampleRate, input.frameCount,
                        SilenceKind::Digital, params, levelForStats, /*statsAreAlreadyDb=*/false);
}

SilenceResult SilenceDetector::detectPerceptual(const SilenceInput& input, const SilenceParameters& params,
                                                 double gateLufs) {
    const std::size_t windowCount = input.momentaryLufs.size();
    if (windowCount == 0) return emptyResult(params);

    const double enterDb = gateLufs;
    const double exitDb  = params.useHysteresis ? gateLufs + params.hysteresisDb : gateLufs;

    // Single "channel": M08's momentary series is already channel-summed.
    std::vector<std::uint8_t> silentPerChannel(windowCount, 0);
    bool silent = true;
    for (std::size_t w = 0; w < windowCount; ++w) {
        const double level = static_cast<double>(input.momentaryLufs[w]);
        // NaN (below the R128 relative gate with nothing to measure) reads as silent (M08: "silence
        // must read NaN/-infinity, never 0" — NaN compares false against everything, so treat it
        // explicitly rather than let the comparisons below silently fall through to "not silent").
        const bool belowEnter = std::isnan(level) || level <= enterDb;
        const bool aboveExit  = !std::isnan(level) && level > exitDb;
        if (silent) {
            if (aboveExit) silent = false;
        } else {
            if (belowEnter) silent = true;
        }
        silentPerChannel[w] = silent ? 1 : 0;
    }

    std::vector<bool>          windowSilent;
    std::vector<std::uint32_t> channelMask;
    combineChannels(silentPerChannel, windowCount, /*channelCount=*/1, ChannelMode::All, windowSilent, channelMask);

    auto runs = runLengthEncode(windowSilent, channelMask);
    mergeRuns(runs, input.momentaryLufsWindowSeconds, params.mergeGapMs);
    discardShortRuns(runs, input.momentaryLufsWindowSeconds, params.minDurationMs);

    auto levelForStats = [&](std::size_t w) {
        const double v = static_cast<double>(input.momentaryLufs[w]);
        return std::isnan(v) ? kNegInf : v;
    };

    // Uses momentaryLufsWindowSeconds (100ms) as the grid — buildResult() derives window-frame size
    // from whichever windowSeconds it's given, so this is consistent with the RMS-grid case.
    return buildResult(std::move(runs), windowCount, input.momentaryLufsWindowSeconds, input.sampleRate,
                        input.frameCount, SilenceKind::Perceptual, params, levelForStats,
                        /*statsAreAlreadyDb=*/true);
}

std::string SilenceResult::toJson() const {
    std::ostringstream out;
    out << "{\"schemaVersion\":\"1.0.0\",";
    out << "\"leadingSilenceSeconds\":" << jsonNumber(leadingSilenceSeconds) << ",";
    out << "\"trailingSilenceSeconds\":" << jsonNumber(trailingSilenceSeconds) << ",";
    out << "\"totalSilenceSeconds\":" << jsonNumber(totalSilenceSeconds) << ",";
    out << "\"silenceFraction\":" << jsonNumber(silenceFraction) << ",";

    out << "\"parametersUsed\":{";
    out << "\"thresholdDb\":" << jsonNumber(parametersUsed.thresholdDb) << ",";
    out << "\"minDurationMs\":" << jsonNumber(parametersUsed.minDurationMs) << ",";
    out << "\"mergeGapMs\":" << jsonNumber(parametersUsed.mergeGapMs) << ",";
    out << "\"channelMode\":\"" << (parametersUsed.channelMode == ChannelMode::Any ? "any" : "all") << "\",";
    out << "\"useHysteresis\":" << (parametersUsed.useHysteresis ? "true" : "false") << ",";
    out << "\"hysteresisDb\":" << jsonNumber(parametersUsed.hysteresisDb);
    out << "},";

    out << "\"regions\":[";
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (i > 0) out << ",";
        const auto& r = regions[i];
        out << "{";
        out << "\"beginFrame\":" << r.range.begin << ",";
        out << "\"endFrame\":" << r.range.end << ",";
        out << "\"startSeconds\":" << jsonNumber(r.startSeconds) << ",";
        out << "\"endSeconds\":" << jsonNumber(r.endSeconds) << ",";
        out << "\"kind\":\"" << kindName(r.kind) << "\",";
        out << "\"position\":\"" << positionName(r.position) << "\",";
        out << "\"peakDbfsWithin\":" << jsonNumber(r.peakDbfsWithin) << ",";
        out << "\"rmsDbfsWithin\":" << jsonNumber(r.rmsDbfsWithin) << ",";
        out << "\"channelMask\":" << r.channelMask;
        out << "}";
    }
    out << "]";

    out << "}";
    return out.str();
}

}  // namespace aud::silence
