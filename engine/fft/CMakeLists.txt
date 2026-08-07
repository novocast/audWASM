# FFT backend interface, PocketFFT wrapper, window functions, STFT. See M06.
#
# Most sources here (windows.cpp, simd_kernels.cpp, stft.cpp) are listed directly in
# engine/CMakeLists.txt's aud_core target, matching the decoder/waveform subsystems' convention (no
# separate library target per subsystem). real_fft.hpp, scaling.hpp, and peak_interp.hpp are
# header-only and need no source entry. naive_dft.{hpp,cpp} is a test-only reference implementation
# (M06 task list: "used only by tests") and is instead listed directly in tests/CMakeLists.txt so it
# never ships in aud_core.
#
# pocketfft_backend.cpp is the one exception, and gets its own tiny target below rather than
# joining aud_core's source list. Vendored pocketfft_hdronly.h throws std::invalid_argument /
# std::runtime_error internally (its shape/stride sanity checks, thread-pool error paths) — a TU
# compiled with aud_core's -fno-exceptions (M00 §2) hard-fails ("exception handling disabled") the
# moment those templates are instantiated for float. Isolating the wrapper here, with default
# exception handling and warnings switched off, sidesteps that exactly the way aud_third_party
# isolates the vendored C decoders from the same project-wide restrictions.
add_library(aud_fft_pocketfft STATIC pocketfft_backend.cpp)

target_compile_features(aud_fft_pocketfft PRIVATE cxx_std_20)

target_include_directories(aud_fft_pocketfft PRIVATE
    ${CMAKE_SOURCE_DIR}/engine
    ${CMAKE_SOURCE_DIR}/third_party
)

if (MSVC)
    target_compile_options(aud_fft_pocketfft PRIVATE /W0)
else()
    target_compile_options(aud_fft_pocketfft PRIVATE -w)
endif()

add_library(aud::fft_pocketfft ALIAS aud_fft_pocketfft)
