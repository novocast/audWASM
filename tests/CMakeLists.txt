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
)

target_link_libraries(aud_tests PRIVATE aud::core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(aud_tests)
