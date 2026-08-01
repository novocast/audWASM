// TODO(M02): libFuzzer target over format_sniffer + all four decoders. Not implemented in the
// initial M00-M02 bootstrap (no libFuzzer/clang toolchain available to validate it there). Sketch
// of the intended shape, left here so it isn't lost:
//
//   extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
//       auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size);
//       auto sessionResult = aud::decoder::DecodeSession::create(bytes);
//       if (!sessionResult.has_value()) return 0;
//       auto session = std::move(sessionResult).value();
//       (void)session.feed(bytes);
//       (void)session.finish();
//       return 0;
//   }
//
// This is deliberately not wired into CMakeLists.txt yet — add an AUD_BUILD_FUZZERS option and a
// -fsanitize=fuzzer,address native-only target once it's ready to run for real (see M02's
// acceptance criterion: "10 million iterations over the corpus with no crash, hang, or ASAN report").
