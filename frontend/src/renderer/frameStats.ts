// Frame budget accounting (M17 "instrumented from the start and surfaces in M19"). A fixed-size
// ring buffer of recent frame times, cheap enough to update unconditionally every rAF frame.

export interface FrameStatsSnapshot {
  count: number;
  averageMs: number;
  p99Ms: number;
  maxMs: number;
  lastMs: number;
}

export class FrameTimeStats {
  private readonly samples: Float64Array;
  private writeIndex = 0;
  private filled = 0;

  constructor(private readonly capacity: number = 300) {
    this.samples = new Float64Array(capacity);
  }

  record(ms: number): void {
    this.samples[this.writeIndex] = ms;
    this.writeIndex = (this.writeIndex + 1) % this.capacity;
    this.filled = Math.min(this.filled + 1, this.capacity);
  }

  snapshot(): FrameStatsSnapshot {
    if (this.filled === 0) return { count: 0, averageMs: 0, p99Ms: 0, maxMs: 0, lastMs: 0 };
    const values = Array.from(this.samples.subarray(0, this.filled)).sort((a, b) => a - b);
    const sum = values.reduce((acc, v) => acc + v, 0);
    const p99Index = Math.min(values.length - 1, Math.floor(values.length * 0.99));
    const lastMs = this.samples[(this.writeIndex - 1 + this.capacity) % this.capacity]!;
    return {
      count: this.filled,
      averageMs: sum / values.length,
      p99Ms: values[p99Index]!,
      maxMs: values[values.length - 1]!,
      lastMs,
    };
  }

  reset(): void {
    this.filled = 0;
    this.writeIndex = 0;
  }
}
