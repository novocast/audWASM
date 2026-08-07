// aud_cli — native driver tool. Not decoration: this is how we profile with perf/VTune, how
// regression fixtures get generated, and how the native-desktop-reuse story stays real. Every
// analyser must be reachable from it (M01).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../../engine/analysis/loudness/loudness_analyzer.hpp"
#include "../../engine/analysis/silence/boundary_refine.hpp"
#include "../../engine/analysis/silence/silence_detector.hpp"
#include "../../engine/analysis/statistics/statistics_analyzer.hpp"
#include "../../engine/decoder/decode_session.hpp"
#include "../../engine/util/diagnostics.hpp"

namespace {

void printUsage() {
    std::printf(
        "aud_cli - audWASM native driver\n"
        "\n"
        "Usage:\n"
        "  aud_cli --version              Print the engine version and build info\n"
        "  aud_cli --self-test            Run the engine self-test, exit 0 on pass\n"
        "  aud_cli decode <file>          Decode a file and print its StreamInfo\n"
        "  aud_cli --report <file>        Decode a file and print its M09 statistics report as JSON\n"
        "  aud_cli --silence <file>       Decode a file and print its M10 silence regions as JSON\n");
}

// Shared by decode/report: reads `path` and runs it through DecodeSession to completion. Returns
// nullptr (having already printed a diagnostic to stderr) on failure.
std::unique_ptr<aud::decoder::DecodeSession> decodeFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "could not open '%s'\n", path);
        return nullptr;
    }
    std::vector<char>      rawBytes(std::istreambuf_iterator<char>(file), (std::istreambuf_iterator<char>()));
    std::vector<std::byte> bytes(rawBytes.size());
    std::transform(rawBytes.begin(), rawBytes.end(), bytes.begin(),
                    [](char c) { return static_cast<std::byte>(c); });

    auto sessionResult = aud::decoder::DecodeSession::create(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!sessionResult.has_value()) {
        std::fprintf(stderr, "sniff/init failed: [%s] %s\n",
                     std::string(aud::toString(sessionResult.error().code)).c_str(), sessionResult.error().detail.c_str());
        return nullptr;
    }
    auto session = std::make_unique<aud::decoder::DecodeSession>(std::move(sessionResult).value());

    auto feedResult = session->feed(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!feedResult.has_value()) {
        std::fprintf(stderr, "decode failed: [%s] %s\n", std::string(aud::toString(feedResult.error().code)).c_str(),
                     feedResult.error().detail.c_str());
        return nullptr;
    }
    auto finishResult = session->finish();
    if (!finishResult.has_value()) {
        std::fprintf(stderr, "decode failed at finish(): [%s] %s\n",
                     std::string(aud::toString(finishResult.error().code)).c_str(), finishResult.error().detail.c_str());
        return nullptr;
    }
    return session;
}

int runVersion() {
    const aud::BuildInfo info = aud::buildInfo();
    std::printf("audWASM engine %s (%s, simd=%s, threads=%s)\n", info.version.c_str(), info.optimisation.c_str(),
                info.simd ? "on" : "off", info.threads ? "on" : "off");
    return 0;
}

int runSelfTest() {
    auto result = aud::selfTest();
    if (!result.has_value()) {
        std::fprintf(stderr, "self-test FAILED: [%s] %s\n", std::string(aud::toString(result.error().code)).c_str(),
                     result.error().detail.c_str());
        return 1;
    }
    std::printf("self-test PASSED\n");
    return 0;
}

int runDecode(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "could not open '%s'\n", path);
        return 1;
    }
    std::vector<char>      rawBytes(std::istreambuf_iterator<char>(file), (std::istreambuf_iterator<char>()));
    std::vector<std::byte> bytes(rawBytes.size());
    std::transform(rawBytes.begin(), rawBytes.end(), bytes.begin(),
                    [](char c) { return static_cast<std::byte>(c); });

    auto sessionResult = aud::decoder::DecodeSession::create(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!sessionResult.has_value()) {
        std::fprintf(stderr, "sniff/init failed: [%s] %s\n",
                     std::string(aud::toString(sessionResult.error().code)).c_str(), sessionResult.error().detail.c_str());
        return 1;
    }
    auto session = std::move(sessionResult).value();

    auto feedResult = session.feed(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!feedResult.has_value()) {
        std::fprintf(stderr, "decode failed: [%s] %s\n", std::string(aud::toString(feedResult.error().code)).c_str(),
                     feedResult.error().detail.c_str());
        return 1;
    }
    auto finishResult = session.finish();
    if (!finishResult.has_value()) {
        std::fprintf(stderr, "decode failed at finish(): [%s] %s\n",
                     std::string(aud::toString(finishResult.error().code)).c_str(), finishResult.error().detail.c_str());
        return 1;
    }

    auto infoResult = session.streamInfo();
    if (!infoResult.has_value()) {
        std::fprintf(stderr, "no stream info available\n");
        return 1;
    }
    const auto& info   = infoResult.value();
    const auto* buffer = session.buffer();
    std::printf(
        "{ \"codec\": \"%s\", \"sampleRate\": %u, \"channels\": %u, \"bitDepth\": %u, \"isLossy\": %s, "
        "\"decodedFrames\": %lld, \"decodedSeconds\": %.3f }\n",
        info.codecName.c_str(), info.sampleRate, info.channels, info.bitDepth, info.isLossy ? "true" : "false",
        static_cast<long long>(buffer != nullptr ? buffer->frameCount() : 0),
        buffer != nullptr ? buffer->durationSeconds() : 0.0);
    return 0;
}

// M09: every analyser must be reachable from the CLI. Decodes the whole file up front (this is a
// batch driver tool, not the progressive-decode UI path), then runs the whole PCM through
// StatisticsAnalyzer chunk-by-chunk exactly as AudioBuffer already has it chunked, and prints the
// resulting report as JSON on stdout (see docs/report-schema.json).
int runReport(const char* path) {
    auto session = decodeFile(path);
    if (!session) {
        return 1;
    }

    auto infoResult = session->streamInfo();
    if (!infoResult.has_value()) {
        std::fprintf(stderr, "no stream info available\n");
        return 1;
    }
    const auto& info   = infoResult.value();
    const auto* buffer = session->buffer();
    if (buffer == nullptr) {
        std::fprintf(stderr, "no decoded PCM available\n");
        return 1;
    }

    aud::statistics::StatisticsResult result;
    aud::statistics::StatisticsConfig config;
    config.containerBitDepth = info.isLossy ? 0 : info.bitDepth;  // lossy codecs have no meaningful integer container

    auto analyzer = aud::statistics::makeStatisticsAnalyzer(result, config);
    const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
    if (auto beginResult = analyzer->begin(spec); !beginResult.has_value()) {
        std::fprintf(stderr, "statistics begin() failed: [%s] %s\n",
                     std::string(aud::toString(beginResult.error().code)).c_str(), beginResult.error().detail.c_str());
        return 1;
    }

    const aud::ChannelIndex channels   = buffer->channelCount();
    const std::size_t       chunkCount = buffer->chunkCount();
    aud::FrameIndex         frameCursor = 0;
    for (std::size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        std::vector<std::span<const aud::Sample>> planar(channels);
        for (aud::ChannelIndex ch = 0; ch < channels; ++ch) {
            planar[ch] = buffer->chunk(ch, chunkIndex);
        }
        const aud::ChunkView view{std::span<const std::span<const aud::Sample>>(planar), frameCursor};
        if (auto processResult = analyzer->process(view); !processResult.has_value()) {
            std::fprintf(stderr, "statistics process() failed: [%s] %s\n",
                         std::string(aud::toString(processResult.error().code)).c_str(),
                         processResult.error().detail.c_str());
            return 1;
        }
        frameCursor += static_cast<aud::FrameIndex>(view.frameCount());
    }

    if (auto finishResult = analyzer->finish(); !finishResult.has_value()) {
        std::fprintf(stderr, "statistics finish() failed: [%s] %s\n",
                     std::string(aud::toString(finishResult.error().code)).c_str(), finishResult.error().detail.c_str());
        return 1;
    }

    std::printf("%s\n", result.toJson().c_str());
    return 0;
}

// M10: every analyser must be reachable from the CLI. Runs the whole file through
// StatisticsAnalyzer (for M09's RMS/digital-silence series) and LoudnessAnalyzer (for M08's
// momentary loudness series, feeding perceptual mode), then SilenceDetector with default
// parameters, then boundary_refine.hpp's sample-precise pass for the threshold and digital regions
// (perceptual has no per-sample test — see boundary_refine.hpp). Prints all three modes' regions
// as one JSON object on stdout.
int runSilence(const char* path) {
    auto session = decodeFile(path);
    if (!session) {
        return 1;
    }

    const auto* buffer = session->buffer();
    if (buffer == nullptr) {
        std::fprintf(stderr, "no decoded PCM available\n");
        return 1;
    }

    const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
    const aud::ChannelIndex channels   = buffer->channelCount();
    const std::size_t       chunkCount = buffer->chunkCount();

    aud::statistics::StatisticsResult statsResult;
    auto statsAnalyzer = aud::statistics::makeStatisticsAnalyzer(statsResult);
    if (auto r = statsAnalyzer->begin(spec); !r.has_value()) {
        std::fprintf(stderr, "statistics begin() failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    aud::loudness::LoudnessResult loudnessResult;
    auto loudnessAnalyzer = aud::loudness::makeLoudnessAnalyzer(loudnessResult);
    if (auto r = loudnessAnalyzer->begin(spec); !r.has_value()) {
        std::fprintf(stderr, "loudness begin() failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    aud::FrameIndex frameCursor = 0;
    for (std::size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
        std::vector<std::span<const aud::Sample>> planar(channels);
        for (aud::ChannelIndex ch = 0; ch < channels; ++ch) {
            planar[ch] = buffer->chunk(ch, chunkIndex);
        }
        const aud::ChunkView view{std::span<const std::span<const aud::Sample>>(planar), frameCursor};
        if (auto r = statsAnalyzer->process(view); !r.has_value()) {
            std::fprintf(stderr, "statistics process() failed: %s\n", r.error().detail.c_str());
            return 1;
        }
        if (auto r = loudnessAnalyzer->process(view); !r.has_value()) {
            std::fprintf(stderr, "loudness process() failed: %s\n", r.error().detail.c_str());
            return 1;
        }
        frameCursor += static_cast<aud::FrameIndex>(view.frameCount());
    }

    if (auto r = statsAnalyzer->finish(); !r.has_value()) {
        std::fprintf(stderr, "statistics finish() failed: %s\n", r.error().detail.c_str());
        return 1;
    }
    if (auto r = loudnessAnalyzer->finish(); !r.has_value()) {
        std::fprintf(stderr, "loudness finish() failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    aud::silence::SilenceInput input;
    input.rmsSeries              = statsResult.rmsSeries;
    input.channelCount           = statsResult.rmsSeriesChannelCount;
    input.digitalSilenceSeries   = statsResult.allZeroSeries;
    input.momentaryLufs           = loudnessResult.momentaryLufs;
    input.sampleRate              = buffer->sampleRate();
    input.frameCount              = buffer->frameCount();

    const aud::silence::SilenceParameters params;  // defaults: -60dBFS, 500ms min, 100ms merge, hysteresis on

    auto thresholdResult  = aud::silence::SilenceDetector::detectThreshold(input, params);
    auto digitalResult    = aud::silence::SilenceDetector::detectDigital(input, params);
    auto perceptualResult = aud::silence::SilenceDetector::detectPerceptual(input, params);

    const std::size_t windowFrames = buffer->sampleRate() == 0 ? 0 : buffer->sampleRate() / 20;
    if (auto r = aud::silence::refineRegionBoundaries(*buffer, thresholdResult.regions, windowFrames,
                                                          std::pow(10.0, params.thresholdDb / 20.0), params.channelMode);
        !r.has_value()) {
        std::fprintf(stderr, "silence boundary refinement (threshold) failed: %s\n", r.error().detail.c_str());
        return 1;
    }
    if (auto r = aud::silence::refineRegionBoundaries(*buffer, digitalResult.regions, windowFrames, 0.0,
                                                          params.channelMode);
        !r.has_value()) {
        std::fprintf(stderr, "silence boundary refinement (digital) failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    std::printf("{\"threshold\":%s,\"digital\":%s,\"perceptual\":%s}\n", thresholdResult.toJson().c_str(),
                digitalResult.toJson().c_str(), perceptualResult.toJson().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    if (std::strcmp(argv[1], "--version") == 0) {
        return runVersion();
    }
    if (std::strcmp(argv[1], "--self-test") == 0) {
        return runSelfTest();
    }
    if (std::strcmp(argv[1], "decode") == 0 && argc >= 3) {
        return runDecode(argv[2]);
    }
    if (std::strcmp(argv[1], "--report") == 0 && argc >= 3) {
        return runReport(argv[2]);
    }
    if (std::strcmp(argv[1], "--silence") == 0 && argc >= 3) {
        return runSilence(argv[2]);
    }
    printUsage();
    return 1;
}
