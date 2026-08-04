# decode_fuzzer — see decode_fuzzer.cpp for the corpus-replay-vs-real-libFuzzer split. This target
# builds the LLVMFuzzerTestOneInput entry point either way; it's only the driver (main() vs
# libFuzzer's own) and the sanitizer flags that change based on toolchain availability.

add_executable(decode_fuzzer decode_fuzzer.cpp)
target_link_libraries(decode_fuzzer PRIVATE aud::core)

enable_testing()  # idempotent — AUD_BUILD_FUZZERS can be ON independently of AUD_BUILD_TESTS
add_test(
    NAME decode_fuzzer_corpus_replay
    COMMAND decode_fuzzer "${CMAKE_SOURCE_DIR}/tests/fixtures"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
