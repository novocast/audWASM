import { describe, expect, it } from 'vitest';
import { buildSincCurve, extractPointSamples } from './waveformLayer.ts';
import type { WaveformBinsLike } from '../renderer.ts';

/** Builds a WaveformBinsLike backed by a plain array, min===max per bin (the 'point' regime's
 *  raw-PCM-fallback shape — see waveformLayer.ts's header comment). */
function binsFrom(values: number[]): WaveformBinsLike {
  return {
    binCount: values.length,
    min: (i) => values[i]!,
    max: (i) => values[i]!,
    rms: (i) => Math.abs(values[i]!),
    absPeak: (i) => Math.abs(values[i]!),
  };
}

describe('extractPointSamples', () => {
  it('collapses a run of identical per-pixel bins to one sample at the run midpoint', () => {
    // 3 pixels/frame: frame A covers pixels 0-2, frame B covers pixels 3-5.
    const bins = binsFrom([0.5, 0.5, 0.5, -0.25, -0.25, -0.25]);
    const samples = extractPointSamples(bins, 0, 6);
    expect(samples).toEqual([
      { pixelX: 1, value: 0.5 },
      { pixelX: 4, value: -0.25 },
    ]);
  });

  it('respects a binOffset into a multi-band buffer', () => {
    const bins = binsFrom([9, 9, 0.1, 0.1]); // band 0 (unused here), band 1 starts at index 2
    const samples = extractPointSamples(bins, 2, 2);
    expect(samples).toEqual([{ pixelX: 0.5, value: 0.1 }]);
  });

  it('returns one sample per pixel when every bin differs (no run to collapse)', () => {
    const bins = binsFrom([0, 0.1, 0.2, 0.3]);
    const samples = extractPointSamples(bins, 0, 4);
    expect(samples.map((s) => s.pixelX)).toEqual([0, 1, 2, 3]);
  });

  it('returns an empty array for zero bins', () => {
    expect(extractPointSamples(binsFrom([]), 0, 0)).toEqual([]);
  });
});

describe('buildSincCurve', () => {
  it('exactly reproduces the sample value at each sample position (sinc(0)=1, sinc(nonzero int)=0)', () => {
    const samples = [
      { pixelX: 0, value: 1 },
      { pixelX: 8, value: -1 },
      { pixelX: 16, value: 0.5 },
    ];
    const curve = buildSincCurve(samples, 8, 24);
    expect(curve[0]).toBeCloseTo(1, 9);
    expect(curve[8]).toBeCloseTo(-1, 9);
    expect(curve[16]).toBeCloseTo(0.5, 9);
  });

  it('overshoots between two opposite-sign samples, unlike a straight-line polyline', () => {
    // A straight line between +1 (pixel 0) and -1 (pixel 8) never leaves [-1, 1]; band-limited
    // (sinc) reconstruction of a signal with a sharp transition characteristically overshoots
    // just past the transition — that overshoot is the whole point of this follow-up (M17 "ties
    // directly to M11's inter-sample peaks").
    const samples = [
      { pixelX: 0, value: 1 },
      { pixelX: 8, value: -1 },
      { pixelX: 16, value: 1 },
      { pixelX: 24, value: -1 },
    ];
    const curve = buildSincCurve(samples, 8, 32);
    const maxAbs = Math.max(...Array.from(curve, Math.abs));
    expect(maxAbs).toBeGreaterThan(1);
  });

  it('returns all zeros when there are no samples', () => {
    const curve = buildSincCurve([], 8, 10);
    expect(Array.from(curve)).toEqual(new Array(10).fill(0));
  });
});
