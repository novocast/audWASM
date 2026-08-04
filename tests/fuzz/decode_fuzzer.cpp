// libFuzzer target over DecodeSession (sniff + all decoders). See M02's acceptance criterion:
// "10 million iterations over the corpus with no crash, hang, or ASAN report".
//
// LLVMFuzzerTestOneInput is the real fuzz entry point and is libFuzzer-ABI compatible — build this
// file with clang's `-fsanitize=fuzzer,address` and no `main()` (define FUZZING_BUILD_MODE_UNSAFE_
// FOR_PRODUCTION) to get genuine coverage-guided fuzzing once a clang+compiler-rt toolchain is
// available. Absent that (this repo currently only has GCC natively and emsdk's clang, which ships
// without native compiler-rt), AUD_BUILD_FUZZERS wires this into a plain corpus-replay executable
// instead: a real regression run of the same entry point, under ASan/UBSan, over tests/fixtures/ —
// not coverage-guided, but not dead code either.

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../engine/decoder/decode_session.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
    auto sessionResult = aud::decoder::DecodeSession::create(bytes);
    if (!sessionResult.has_value()) {
        return 0;
    }
    auto session = std::move(sessionResult).value();
    (void)session.feed(bytes);
    (void)session.finish();
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

// Corpus-replay driver: runs LLVMFuzzerTestOneInput over every regular file under each directory
// passed on argv (default: tests/fixtures relative to cwd). Not coverage-guided — a plain smoke
// pass over real fixture bytes so this entry point is exercised by *something* until real libFuzzer
// is available.
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
            std::fprintf(stderr, "decode_fuzzer: corpus path does not exist: %s\n", root.string().c_str());
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

    std::printf("decode_fuzzer: replayed %zu corpus file(s) with no crash/ASAN report\n", filesRun);
    return filesRun == 0 ? 1 : 0;
}

#endif  // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
