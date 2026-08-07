# Analyzer interface + registry, stats, silence, clipping, dc, loudness, beats, transients. See
# M08-M14, M20.
# analyzer.hpp (the Analyzer interface, M00 §6) is header-only and needs no source entry.
# loudness/ (M08), statistics/ (M09), silence/ (M10), clipping/ (M11), dc/ (M12), beats/ (M13) and
# transients/ (M14) land their sources directly in aud_core's list in the parent CMakeLists.txt,
# same convention as decoder/waveform/spectrogram — no dedicated target needed here.
# The registry (M20) is what's left.
