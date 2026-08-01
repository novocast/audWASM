# aud::core — static library, platform-independent (must never depend on Emscripten headers; see
# M00 §4 and the `lint` CI job's grep check).

add_library(aud_core STATIC
    util/result.cpp
    util/logging.cpp
    util/assert.cpp
    util/allocator.cpp
    util/audio_buffer.cpp
    util/platform.cpp

    decoder/format_sniffer.cpp
    decoder/sample_convert.cpp
    decoder/wav_decoder.cpp
    decoder/flac_decoder.cpp
    decoder/mp3_decoder.cpp
    decoder/vorbis_decoder.cpp
    decoder/external_pcm_source.cpp
    decoder/decode_session.cpp
)

target_include_directories(aud_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aud_core
    PUBLIC  aud::project_options
    PRIVATE aud::third_party
)

if (AUD_TARGETING_WASM AND CMAKE_BUILD_TYPE STREQUAL "Release")
    # Decision (M01): release WASM uses -O3 for the engine; -Oz is rejected. Analysis throughput
    # matters more than the ~15% size delta. Revisit with real numbers in M22.
    target_compile_options(aud_core PUBLIC -O3)
endif()

# -fno-exceptions -fno-rtti for aud::core's own translation units (M00 §2). Deliberately PRIVATE,
# not PUBLIC: Catch2 (aud_tests) needs exceptions for its own assertion machinery, and we don't want
# to force every consumer of aud::core into the same restriction — only the engine's own code is
# required to avoid throw/dynamic_cast/typeid.
if (MSVC)
    target_compile_options(aud_core PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(aud_core PRIVATE -fno-exceptions -fno-rtti)
endif()

add_library(aud::core ALIAS aud_core)

# Placeholder subsystems — no sources yet, added as their milestones land. Kept as subdirectories
# (rather than bare folders) so each has its own CMakeLists.txt from day one, per the M00 task list.
add_subdirectory(analysis)
add_subdirectory(fft)
add_subdirectory(waveform)
add_subdirectory(cache)
add_subdirectory(metadata)
add_subdirectory(playback)
