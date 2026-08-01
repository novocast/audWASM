// aud_cli — native driver tool. Not decoration: this is how we profile with perf/VTune, how
// regression fixtures get generated, and how the native-desktop-reuse story stays real. Every
// analyser must be reachable from it (M01).

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

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
        "  aud_cli decode <file>          Decode a file and print its StreamInfo\n");
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
    std::vector<std::byte> bytes(std::istreambuf_iterator<char>(file), (std::istreambuf_iterator<char>()));

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
    printUsage();
    return 1;
}
