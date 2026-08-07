// Typed-array view construction over the tile/overview byte buffers M07's Embind surface hands
// back as raw heap pointers. Growth-safe like waveformView.ts: never cache the returned view across
// a call that can allocate — re-derive it via this module each time, and copy out (`.slice()`)
// before handing bytes across a postMessage boundary (the spectrogram worker's whole job).

import type { AudModule } from './aud_wasm.d.ts';
import { uint8View } from './heap_view.ts';

/** Mirrors engine/spectrogram/tile.hpp: kTileWidth * kTileHeight quantised dB bytes, row 0 = lowest
 *  displayed frequency. */
export const kTileWidth = 256;
export const kTileHeight = 256;
export const kTileByteLength = kTileWidth * kTileHeight;

/** Zero-copy view of one tile's quantised bytes. Only valid until the next allocating Module call —
 *  the caller (spectrogramWorker.ts) must copy out (`.slice()`) before posting it to the main
 *  thread, since a Uint8Array subarray of the WASM heap can't be transferred across a worker
 *  boundary (its backing ArrayBuffer is the whole heap, not an isolated allocation).
 */
export function tileBytesView(module: AudModule, ptr: number, byteLength: number): Uint8Array {
  return uint8View(module, ptr, byteLength);
}

export function overviewBytesView(module: AudModule, ptr: number, width: number, height: number): Uint8Array {
  return uint8View(module, ptr, width * height);
}
