# Transport state machine, ring buffer, resampler. See M03.
#
# Sources (resampler.cpp, transport.cpp) are listed directly in engine/CMakeLists.txt's aud_core
# target, matching the decoder/ subsystem's convention (no separate library target per subsystem).
# fade.hpp and ring_buffer.hpp are header-only and need no source entry.
