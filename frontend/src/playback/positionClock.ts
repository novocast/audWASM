// Latency-corrected, interpolated, smoothed playback position for a 60fps UI cursor. See M03
// "Position accuracy":
//
//   displayedPosition = sourceFramesConsumed - latencySeconds * sourceSampleRate
//
// and: "Between animation frames the position is interpolated from performance.now() deltas rather
// than re-read, to avoid a jittery cursor, and re-synced to the atomic every frame with a small
// smoothing factor (a one-pole filter, not a hard snap)."
//
// Concretely: each tick() predicts where the position "should" be from the elapsed wall-clock time
// since the last tick, then blends a small fraction of the way toward the freshly-read
// authoritative source-frame count. A hard snap only happens on reset() (seek), where the
// discontinuity is real and expected.

export interface PositionClockOptions {
  sourceSampleRate: number;
  /** e.g. `() => transport.producedSourceFrame() - ring.framesAvailableToRead()`. */
  getAuthoritativeSourceFrame(): number;
  /** `AudioContext.outputLatency ?? baseLatency ?? 0`. */
  getLatencySeconds(): number;
  /** One-pole filter coefficient in (0, 1]; higher tracks the authoritative value faster but
   *  jitters more, lower is smoother but lags more. */
  smoothingFactor?: number;
}

export class PositionClock {
  private estimatedFrame = 0;
  private lastTickMs: number | null = null;
  private readonly smoothingFactor: number;

  constructor(private readonly opts: PositionClockOptions) {
    this.smoothingFactor = opts.smoothingFactor ?? 0.2;
  }

  /** Call once per animation frame with `performance.now()`. Returns the current estimated
   *  position in source-frame units (divide by sourceSampleRate for seconds). */
  tick(nowMs: number): number {
    const rate = this.opts.sourceSampleRate;
    const authoritative =
      this.opts.getAuthoritativeSourceFrame() - this.opts.getLatencySeconds() * rate;

    if (this.lastTickMs === null) {
      this.estimatedFrame = authoritative;
      this.lastTickMs = nowMs;
      return this.estimatedFrame;
    }

    const dtSeconds = Math.max(0, nowMs - this.lastTickMs) / 1000;
    const predicted = this.estimatedFrame + dtSeconds * rate;
    this.estimatedFrame = predicted + this.smoothingFactor * (authoritative - predicted);
    this.lastTickMs = nowMs;
    return this.estimatedFrame;
  }

  get positionSeconds(): number {
    return this.estimatedFrame / this.opts.sourceSampleRate;
  }

  /** Hard snap — used on seek, where the jump is a real discontinuity, not something to smooth
   *  through. */
  reset(sourceFrame: number): void {
    this.estimatedFrame = sourceFrame;
    this.lastTickMs = null;
  }
}
