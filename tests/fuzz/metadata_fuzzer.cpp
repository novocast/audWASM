// libFuzzer target over aud::metadata::extract() — every tag parser (ID3v2, ID3v1, APEv2, Vorbis
// comment, FLAC metadata blocks, MP4 ilst, RIFF/bext/iXML) runs on arbitrary bytes. See M15's
// headline acceptance criterion: "the fuzzer runs 10 million iterations across all parsers with no
// crash, hang, OOM or ASAN report" — metadata parsing is named as the highest-risk attack surface
// in the project (tag data is attacker-controlled and is historically the source of most audio-
// library CVEs).
//
// Same corpus-replay-vs-real-libFuzzer split as decode_fuzzer.cpp — see that file's header comment
// for why; this target follows the identical pattern.

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../engine/metadata/metadata.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto bytes  = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
    auto result = aud::metadata::extract(bytes);
    // extract() is designed to never fail (an untagged/garbage file is a valid empty result, not
    // an Error) — but if that ever changes, touching .value() here would be the crash that catches
    // it, which is exactly what a fuzz target should do.
    if (result.has_value()) {
        (void)result.value().toJson();  // also exercise the JSON serialiser over whatever came out
    }
    return 0;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto    size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<std::uint8_t> bytes(size);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

}  // namespace

// Corpus-replay driver — identical shape to decode_fuzzer.cpp's, over tests/fixtures/ by default.
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> roots;
    for (int i = 1; i < argc; ++i) {
        roots.emplace_back(argv[i]);
    }
    if (roots.empty()) {
        roots.emplace_back("tests/fixtures");
    }

    std::size_t filesRun = 0;
    for (const auto& root : roots) {
        if (!std::filesystem::exists(root)) {
            std::fprintf(stderr, "metadata_fuzzer: corpus path does not exist: %s\n", root.string().c_str());
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            auto bytes = readFile(entry.path());
            LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
            ++filesRun;
        }
    }

    std::printf("metadata_fuzzer: replayed %zu corpus file(s) with no crash/ASAN report\n", filesRun);
    return filesRun == 0 ? 1 : 0;
}

#endif  // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
