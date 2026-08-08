// The only place allowed to touch Module.HEAPF32/HEAPU8/HEAPU32 directly (M01 risk mitigation:
// "memory-growth invalidating views causes heisenbugs"). ALLOW_MEMORY_GROWTH=1 means any call that
// can allocate invalidates every outstanding typed-array view; callers must re-acquire through
// this module after such a call, never cache a raw view across one.

import type { AudModule } from './aud_wasm.d.ts';

let growthCounter = 0;

/**
 * Call this immediately after any Module call that might allocate (feedBytes, finish, etc.) so
 * views taken before it are provably stale. In dev builds this is what backs the "stale-view use"
 * assertion mentioned in M01's risk table; in release it's a no-op counter bump.
 */
export function noteGrowthBoundary(): void {
  growthCounter += 1;
}

export function currentGrowthEpoch(): number {
  return growthCounter;
}

/** Copies `bytes` into a freshly `_malloc`'d heap region and returns { ptr, length }. Caller must
 *  `Module._free(ptr)` once the native side is done reading it (immediately after feedBytes, since
 *  the engine copies into its own AudioBuffer/decoder-internal storage synchronously). */
export function copyIntoHeap(module: AudModule, bytes: Uint8Array): { ptr: number; length: number } {
  const ptr = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, ptr);
  noteGrowthBoundary();
  return { ptr, length: bytes.length };
}

/** Zero-copy Float32 view of a heap region. The returned view is only valid until the next
 *  allocating Module call — re-acquire via this function, never hold across one. */
export function float32View(module: AudModule, ptr: number, lengthInFloats: number): Float32Array {
  return module.HEAPF32.subarray(ptr / 4, ptr / 4 + lengthInFloats);
}

/** Copies `floats` into a freshly `_malloc`'d heap region and returns { ptr, length }. Caller must
 *  `Module._free(ptr)` once the native side is done reading it. */
export function copyFloat32IntoHeap(module: AudModule, floats: Float32Array): { ptr: number; length: number } {
  const ptr = module._malloc(floats.length * 4);
  module.HEAPF32.set(floats, ptr / 4);
  noteGrowthBoundary();
  return { ptr, length: floats.length };
}

/** Zero-copy Uint8 view of a heap region (spectrogram tile bytes, etc). Same growth-boundary
 *  discipline as float32View — re-acquire, never cache across an allocating call. */
export function uint8View(module: AudModule, ptr: number, length: number): Uint8Array {
  return module.HEAPU8.subarray(ptr, ptr + length);
}

/** Zero-copy Float64 view of a heap region (M08 loudness's per-channel peak arrays, which are
 *  std::vector<double> on the engine side). Same growth-boundary discipline as float32View. */
export function float64View(module: AudModule, ptr: number, lengthInDoubles: number): Float64Array {
  return module.HEAPF64.subarray(ptr / 8, ptr / 8 + lengthInDoubles);
}

/** Copies `doubles` into a freshly `_malloc`'d heap region and returns { ptr, length }. Caller must
 *  `Module._free(ptr)` once the native side is done reading it (Transients.create()'s onset-time
 *  handoff — see M13/M14's HEAPF64 onset-times convention). */
export function copyFloat64IntoHeap(module: AudModule, doubles: Float64Array): { ptr: number; length: number } {
  const ptr = module._malloc(doubles.length * 8);
  module.HEAPF64.set(doubles, ptr / 8);
  noteGrowthBoundary();
  return { ptr, length: doubles.length };
}

/** Zero-copy Uint32 view of a heap region (M09 statistics' per-channel histogram, a
 *  std::array<uint32_t, 1024> on the engine side). Same growth-boundary discipline as float32View. */
export function uint32View(module: AudModule, ptr: number, lengthInU32: number): Uint32Array {
  return module.HEAPU32.subarray(ptr / 4, ptr / 4 + lengthInU32);
}

/** WASM32 pointers are 4-byte values readable/writable through HEAPU32. Copies `pointers` (each a
 *  heap address returned by a prior _malloc, e.g. from copyFloat32IntoHeap) into a freshly
 *  `_malloc`'d array-of-pointers and returns its heap address — the "pointer to pointers" bulk
 *  handoff PcmBuffer.appendPlanar() and similar multi-channel APIs take. Caller must `_free` it. */
export function copyPointerArrayIntoHeap(module: AudModule, pointers: readonly number[]): number {
  const ptr = module._malloc(pointers.length * 4);
  module.HEAPU32.set(pointers, ptr / 4);
  noteGrowthBoundary();
  return ptr;
}
