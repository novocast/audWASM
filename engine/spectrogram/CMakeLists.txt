# Tile-based spectrogram generation (M07). Sources are listed directly in engine/CMakeLists.txt's
# aud_core target, matching the fft/waveform subsystems' convention (no separate library target per
# subsystem). tile.hpp, tile_generator.hpp, tile_cache.hpp, freq_mapping.hpp, frame_computer.hpp,
# overview.hpp, and point_query.hpp are all headers with a matching .cpp except tile.hpp itself,
# which is header-only (just the TileKey/TileConfig/quantisation types) and needs no source entry.
