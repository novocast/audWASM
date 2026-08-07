# .awc reader/writer, chunk registry, hashing. See M16.

target_sources(aud_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hash.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/awc_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/awc_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cache_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_loudness.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_waveform.cpp
)

target_sources(aud_core PUBLIC FILE_SET HEADERS
    FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/serialise.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hash.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/awc_writer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/awc_reader.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cache_manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_loudness.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/chunk_waveform.hpp
)
