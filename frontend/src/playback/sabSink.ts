// SharedArrayBuffer-backed lock-free SPSC ring, mirroring engine/playback/ring_buffer.hpp's design
// (monotonic read/write counters, power-of-two capacity, mask-based indexing) — reimplemented here
// in TS rather than shared via a WASM instance because the two ends of this ring live in genuinely
// different JS realms (a Worker producing, an AudioWorkletGlobalScope consuming) that don't share a
// WASM module instance. See playback_bindings.cpp's file header for the fuller architecture note.
//
// Layout of the backing SharedArrayBuffer:
//   [0, 8)                                  writeIndex (BigInt64, Atomics)
//   [8, 16)                                 readIndex  (BigInt64, Atomics)
//   [16, 16 + channelCount*capacity*4)       channel 0..N-1 float32 data, each capacity frames long
//
// BigInt64 counters (rather than Int32) so a monotonically-increasing frame count never wraps
// within any realistic session length — the M03 doc's "60 minutes of playback for drift" test and
// beyond are all well inside 2^63 frames.

import type { PcmSinkReader, PcmSinkWriter } from './pcmSink.ts';

const kHeaderBytes = 16;

function roundUpToPowerOfTwo(v: number): number {
  let p = 1;
  while (p < v) p <<= 1;
  return p;
}

interface RingLayout {
  writeIndex: BigInt64Array;
  readIndex: BigInt64Array;
  channels: Float32Array[];
  capacity: number;
  mask: number;
}

function layoutOf(buffer: SharedArrayBuffer, channelCount: number, capacity: number): RingLayout {
  const writeIndex = new BigInt64Array(buffer, 0, 1);
  const readIndex = new BigInt64Array(buffer, 8, 1);
  const channels: Float32Array[] = [];
  for (let ch = 0; ch < channelCount; ch++) {
    channels.push(new Float32Array(buffer, kHeaderBytes + ch * capacity * 4, capacity));
  }
  return { writeIndex, readIndex, channels, capacity, mask: capacity - 1 };
}

/** Allocates a fresh ring. `capacityFrames` is a minimum; actual capacity rounds up to a power of
 *  two, matching RingBuffer's C++ counterpart. */
export function allocateSabRing(
  channelCount: number,
  capacityFrames: number,
): { buffer: SharedArrayBuffer; capacity: number } {
  const capacity = roundUpToPowerOfTwo(capacityFrames);
  const buffer = new SharedArrayBuffer(kHeaderBytes + channelCount * capacity * 4);
  return { buffer, capacity };
}

export class SabRingWriter implements PcmSinkWriter {
  private readonly layout: RingLayout;

  constructor(
    buffer: SharedArrayBuffer,
    public readonly channelCount: number,
    public readonly capacityFrames: number,
  ) {
    this.layout = layoutOf(buffer, channelCount, capacityFrames);
  }

  framesAvailableToWrite(): number {
    const w = Atomics.load(this.layout.writeIndex, 0);
    const r = Atomics.load(this.layout.readIndex, 0);
    return this.layout.capacity - Number(w - r);
  }

  write(planar: Float32Array[], frameCount: number): number {
    const toWrite = Math.min(frameCount, this.framesAvailableToWrite());
    const writeIdx = Atomics.load(this.layout.writeIndex, 0);
    const base = Number(writeIdx);
    for (let ch = 0; ch < this.channelCount; ch++) {
      const dst = this.layout.channels[ch]!;
      const src = planar[ch]!;
      for (let i = 0; i < toWrite; i++) {
        dst[(base + i) & this.layout.mask] = src[i]!;
      }
    }
    Atomics.store(this.layout.writeIndex, 0, writeIdx + BigInt(toWrite));
    return toWrite;
  }

  reset(): void {
    Atomics.store(this.layout.readIndex, 0, 0n);
    Atomics.store(this.layout.writeIndex, 0, 0n);
  }
}

export class SabRingReader implements PcmSinkReader {
  private readonly layout: RingLayout;

  constructor(
    buffer: SharedArrayBuffer,
    public readonly channelCount: number,
    capacityFrames: number,
  ) {
    this.layout = layoutOf(buffer, channelCount, capacityFrames);
  }

  framesAvailableToRead(): number {
    const w = Atomics.load(this.layout.writeIndex, 0);
    const r = Atomics.load(this.layout.readIndex, 0);
    return Number(w - r);
  }

  read(planarOut: Float32Array[], frameCount: number): number {
    const toRead = Math.min(frameCount, this.framesAvailableToRead());
    const readIdx = Atomics.load(this.layout.readIndex, 0);
    const base = Number(readIdx);
    for (let ch = 0; ch < this.channelCount; ch++) {
      const src = this.layout.channels[ch]!;
      const dst = planarOut[ch]!;
      for (let i = 0; i < toRead; i++) {
        dst[i] = src[(base + i) & this.layout.mask]!;
      }
    }
    Atomics.store(this.layout.readIndex, 0, readIdx + BigInt(toRead));
    return toRead;
  }
}
