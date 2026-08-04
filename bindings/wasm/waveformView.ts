// Typed-array view construction over the WaveformBin[] buffers M04's Embind surface hands back as
// raw heap pointers. Growth-safe like heap_view.ts: never cache the returned view across a call
// that can allocate (ALLOW_MEMORY_GROWTH=1 invalidates every outstanding HEAPF32 view) — re-derive
// it from the module + pointer each time you need it instead.

import type { AudModule } from './aud_wasm.d.ts';

/** Mirrors engine/waveform/waveform_bin.hpp's WaveformBin: 4 float32s, in this field order. */
export const kWaveformBinFloats = 4;

export interface WaveformBinsView {
  readonly binCount: number;
  min(i: number): number;
  max(i: number): number;
  rms(i: number): number;
  absPeak(i: number): number;
}

/** Zero-copy view of `binCount` WaveformBins starting at heap pointer `ptr`. Only valid until the
 *  next allocating Module call — re-acquire via this function, never hold across one. */
export function waveformBinsView(module: AudModule, ptr: number, binCount: number): WaveformBinsView {
  const floats = module.HEAPF32.subarray(ptr / 4, ptr / 4 + binCount * kWaveformBinFloats);
  return {
    binCount,
    min: (i) => floats[i * kWaveformBinFloats + 0]!,
    max: (i) => floats[i * kWaveformBinFloats + 1]!,
    rms: (i) => floats[i * kWaveformBinFloats + 2]!,
    absPeak: (i) => floats[i * kWaveformBinFloats + 3]!,
  };
}
