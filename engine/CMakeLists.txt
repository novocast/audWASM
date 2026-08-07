# aud::core — static library, platform-independent (must never depend on Emscripten headers; see
# M00 §4 and the `lint` CI job's grep check).

add_library(aud_core STATIC
    util/result.cpp
    util/logging.cpp
    util/assert.cpp
    util/allocator.cpp
    util/audio_buffer.cpp
    util/platform.cpp
    util/diagnostics.cpp

    decoder/decoder.cpp
    decoder/format_sniffer.cpp
    decoder/sample_convert.cpp
    decoder/wav_decoder.cpp
    decoder/flac_decoder.cpp
    decoder/mp3_decoder.cpp
    decoder/vorbis_decoder.cpp
    decoder/external_pcm_source.cpp
    decoder/decode_session.cpp

    playback/resampler.cpp
    playback/transport.cpp

    analysis/loudness/k_weighting.cpp
    analysis/loudness/channel_weights.cpp
    analysis/loudness/block_accumulator.cpp
    analysis/loudness/gating.cpp
    analysis/loudness/lra.cpp
    analysis/loudness/true_peak.cpp
    analysis/loudness/loudness_analyzer.cpp

    analysis/statistics/bit_depth.cpp
    analysis/statistics/stereo.cpp
    analysis/statistics/dynamic_range.cpp
    analysis/statistics/statistics_analyzer.cpp

    analysis/silence/silence_detector.cpp
    analysis/silence/boundary_refine.cpp

    analysis/clipping/clip_detector.cpp
    analysis/clipping/limiting_heuristic.cpp

    analysis/dc/dc_analyzer.cpp

    analysis/beats/odf.cpp
    analysis/beats/whitening.cpp
    analysis/beats/normalise.cpp
    analysis/beats/peak_pick.cpp
    analysis/beats/tempo.cpp
    analysis/beats/beat_tracker.cpp
    analysis/beats/beat_analyzer.cpp

    analysis/transients/refine_timing.cpp
    analysis/transients/features.cpp
    analysis/transients/classifier.cpp
    analysis/transients/click_detector.cpp
    analysis/transients/dropout_detector.cpp
    analysis/transients/transient_analyzer.cpp

    fft/windows.cpp
    fft/simd_kernels.cpp
    fft/stft.cpp

    waveform/reduce.cpp
    waveform/pyramid.cpp
    waveform/waveform_analyzer.cpp
    waveform/waveform_store.cpp

    spectrogram/frame_computer.cpp
    spectrogram/freq_mapping.cpp
    spectrogram/tile_generator.cpp
    spectrogram/tile_cache.cpp
    spectrogram/overview.cpp
    spectrogram/point_query.cpp

    metadata/metadata.cpp
    metadata/tag_set.cpp
    metadata/text_encoding.cpp
    metadata/pictures.cpp
    metadata/lyrics.cpp
    metadata/cue.cpp
    metadata/replaygain.cpp
    metadata/id3v1.cpp
    metadata/id3v2.cpp
    metadata/apev2.cpp
    metadata/vorbis_comment.cpp
    metadata/riff.cpp
    metadata/mp4/box_reader.cpp
    metadata/mp4/ilst.cpp
    metadata/metadata_extractor.cpp
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
add_subdirectory(spectrogram)
add_subdirectory(cache)
add_subdirectory(metadata)
add_subdirectory(playback)

# aud_fft_pocketfft is defined inside fft/'s subdirectory (see fft/CMakeLists.txt for why it isn't
# just another source in aud_core's list above); wire it in now that add_subdirectory(fft) above
# has declared it.
target_link_libraries(aud_core PRIVATE aud::fft_pocketfft)
