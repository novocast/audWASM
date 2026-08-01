// The only place allowed to touch Module.HEAPF32/HEAPU8/HEAPU32 directly (M01 risk mitigation:
// "memory-growth invalidating views causes heisenbugs"). ALLOW_MEMORY_GROWTH=1 means any call that
// can allocate invalidates every outstanding typed-array view; callers must re-acquire through
// this module after such a call, never cache a raw view across one.

import type { AudModule } from './engine.d.ts';

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
