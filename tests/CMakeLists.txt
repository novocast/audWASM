include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)
FetchContent_MakeAvailable(Catch2)

add_executable(aud_tests
    unit/test_result.cpp
    unit/test_audio_buffer.cpp
    unit/test_chunk_reader.cpp
    unit/test_sample_convert.cpp
    unit/test_format_sniffer.cpp
    unit/test_decode_session.cpp
    unit/test_ring_buffer.cpp
    unit/test_resampler.cpp
    unit/test_transport.cpp
    unit/test_waveform_reduce.cpp
    unit/test_waveform_analyzer.cpp
    unit/test_waveform_store.cpp
    unit/test_waveform_pyramid.cpp

    # naive_dft.cpp is the O(N^2) reference DFT (M06 task list: "used only by tests") — listed here
    # rather than in aud_core's source list so it never ships (see engine/fft/CMakeLists.txt).
    ../engine/fft/naive_dft.cpp
    unit/test_fft_backend.cpp
    unit/test_fft_windows.cpp
    unit/test_fft_scaling.cpp
    unit/test_stft.cpp

    unit/test_freq_mapping.cpp
    unit/test_tile_generator.cpp
    unit/test_tile_cache.cpp
    unit/test_point_query.cpp

    unit/test_k_weighting.cpp
    unit/test_channel_weights.cpp
    unit/test_gating.cpp
    unit/test_lra.cpp
    unit/test_true_peak.cpp
    unit/test_loudness_analyzer.cpp

    unit/test_pairwise_summation.cpp
    unit/test_statistics_accumulator.cpp
    unit/test_bit_depth.cpp
    unit/test_stereo.cpp
    unit/test_dynamic_range.cpp
    unit/test_statistics_analyzer.cpp

    unit/test_silence_detector.cpp
    unit/test_boundary_refine.cpp

    unit/test_limiting_heuristic.cpp
    unit/test_clip_detector.cpp

    unit/test_dc_analyzer.cpp

    unit/test_beats_odf.cpp
    unit/test_beats_peak_pick.cpp
    unit/test_beats_tempo.cpp
    unit/test_beats_beat_tracker.cpp
    unit/test_beats_analyzer.cpp

    unit/test_transients_analyzer.cpp

    unit/test_metadata_text_encoding.cpp
    unit/test_metadata_id3v2.cpp
    unit/test_metadata_id3v1_apev2.cpp
    unit/test_metadata_vorbis_flac.cpp
    unit/test_metadata_mp4.cpp
    unit/test_metadata_riff.cpp
    unit/test_metadata_lyrics.cpp
    unit/test_metadata_extractor.cpp
)

target_link_libraries(aud_tests PRIVATE aud::core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(aud_tests)
