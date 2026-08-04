# Peak/RMS extraction, mipmap pyramid. See M04, M05.
#
# Sources (reduce.cpp, waveform_analyzer.cpp, waveform_store.cpp) are listed directly in
# engine/CMakeLists.txt's aud_core target, matching the decoder/playback subsystems' convention (no
# separate library target per subsystem). waveform_bin.hpp is header-only and needs no source entry.
