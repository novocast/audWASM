# Vendored single-header/single-file decoders.
#
# Built as its own target with warnings OFF and linked PRIVATE by engine/decoder — see M01 decision
# that third-party code must never be subject to aud_project_options / -Werror.

add_library(aud_third_party STATIC
    dr_wav_impl.c
    dr_flac_impl.c
    dr_mp3_impl.c
    stb_vorbis.c
)

target_include_directories(aud_third_party PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# stb_vorbis.c is written as C89-ish C; dr_libs impls are C99. Neither wants our project warning
# flags or -Werror. Compile as plain C with warnings suppressed.
set_target_properties(aud_third_party PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    LINKER_LANGUAGE C
)

if (MSVC)
    target_compile_options(aud_third_party PRIVATE /W0 /wd4996)
else()
    target_compile_options(aud_third_party PRIVATE -w)
endif()

# stb_vorbis relies on the pushdata API for streaming decode (M02 decision); nothing to define here,
# it is always compiled in unless STB_VORBIS_NO_PUSHDATA_API is set, which we never set.

add_library(aud::third_party ALIAS aud_third_party)
