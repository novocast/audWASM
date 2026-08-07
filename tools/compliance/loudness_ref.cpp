// Standalone driver for M08's compliance/cross-validation pass — NOT part of the shipped engine,
// aud_cli, or CI. Decodes a file exactly the way aud_cli's `decode` command does (DecodeSession),
// then runs it through the real aud::loudness::LoudnessAnalyzer the same way loudness_bindings.cpp
// does, and prints the result as CSV so it can be diffed against ffmpeg's `-af ebur128` and
// libebur128's numbers for the same file (see tools/compliance/ebur128_ref.c and
// tools/compliance/run_compliance.sh).
//
// Usage: loudness_ref <audio_file>
// Build (from the repo root, against an existing native-debug build's static libs):
//   g++ -std=c++20 -O2 -I engine tools/compliance/loudness_ref.cpp \
//       build/native-debug-wsl/engine/libaud_core.a \
//       build/native-debug-wsl/third_party/libaud_third_party.a \
//       -o /tmp/loudness_ref

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

#include "analysis/loudness/loudness_analyzer.hpp"
#include "decoder/decode_session.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <audio_file>\n", argv[0]);
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "could not open '%s'\n", argv[1]);
        return 1;
    }
    std::vector<char>      rawBytes(std::istreambuf_iterator<char>(file), (std::istreambuf_iterator<char>()));
    std::vector<std::byte> bytes(rawBytes.size());
    std::transform(rawBytes.begin(), rawBytes.end(), bytes.begin(),
                    [](char c) { return static_cast<std::byte>(c); });

    auto sessionResult = aud::decoder::DecodeSession::create(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!sessionResult.has_value()) {
        std::fprintf(stderr, "sniff/init failed: %s\n", sessionResult.error().detail.c_str());
        return 1;
    }
    auto session = std::move(sessionResult).value();

    if (auto r = session.feed(std::span<const std::byte>(bytes.data(), bytes.size())); !r.has_value()) {
        std::fprintf(stderr, "decode failed: %s\n", r.error().detail.c_str());
        return 1;
    }
    if (auto r = session.finish(); !r.has_value()) {
        std::fprintf(stderr, "decode failed at finish(): %s\n", r.error().detail.c_str());
        return 1;
    }

    const auto* buffer = session.buffer();
    if (buffer == nullptr) {
        std::fprintf(stderr, "no decoded buffer\n");
        return 1;
    }

    aud::loudness::LoudnessResult   result;
    std::unique_ptr<aud::Analyzer>  analyzer = aud::loudness::makeLoudnessAnalyzer(result);

    const aud::AudioSpec spec{buffer->sampleRate(), buffer->channelCount(), buffer->frameCount()};
    if (auto r = analyzer->begin(spec); !r.has_value()) {
        std::fprintf(stderr, "begin() failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    for (std::size_t c = 0; c < buffer->chunkCount(); ++c) {
        std::vector<std::span<const aud::Sample>> planar(buffer->channelCount());
        for (aud::ChannelIndex ch = 0; ch < buffer->channelCount(); ++ch) {
            planar[ch] = buffer->chunk(ch, c);
        }
        const aud::ChunkView view{std::span<const std::span<const aud::Sample>>(planar), 0};
        if (auto r = analyzer->process(view); !r.has_value()) {
            std::fprintf(stderr, "process() failed: %s\n", r.error().detail.c_str());
            return 1;
        }
    }
    if (auto r = analyzer->finish(); !r.has_value()) {
        std::fprintf(stderr, "finish() failed: %s\n", r.error().detail.c_str());
        return 1;
    }

    std::printf("%.6f,%.6f,%.6f,%.6f,%u,%d\n", result.integratedLufs, result.loudnessRangeLu,
                result.truePeakDbtp, result.samplePeakDbfs, result.truePeakOversampling,
                result.usedFallbackChannelLayout ? 1 : 0);

    // Two extra lines: the full momentary and short-term series (100 ms resolution), so callers
    // that need "Max M"/"Max S" over a single file (EBU Tech 3341 cases 10/13) can compute it
    // exactly rather than approximate it from the integrated value.
    for (std::size_t i = 0; i < result.momentaryLufs.size(); ++i) {
        std::printf("%s%.6f", i == 0 ? "" : ",", static_cast<double>(result.momentaryLufs[i]));
    }
    std::printf("\n");
    for (std::size_t i = 0; i < result.shortTermLufs.size(); ++i) {
        std::printf("%s%.6f", i == 0 ? "" : ",", static_cast<double>(result.shortTermLufs[i]));
    }
    std::printf("\n");
    return 0;
}
