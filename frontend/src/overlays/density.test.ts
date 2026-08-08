import { describe, expect, it } from 'vitest';
import { aggregateToPixels, regionsToPixelSpans, resampleCurveToPixels, thinToPixels } from './density.ts';
import type { Marker, CurveSeries } from './model.ts';

const view = { startFrame: 0, framesPerPixel: 100 };

function clip(id: string, frame: number, severity?: Marker['severity']): Marker {
  return { id, kind: 'clipping', startFrame: frame, ...(severity !== undefined ? { severity } : {}) };
}

describe('aggregateToPixels', () => {
  it('preserves a single isolated event in a dense pixel (the "do not lose the needle" case)', () => {
    // One clip amid a track otherwise silent: must still surface as count 1, not be averaged away.
    const markers = [clip('lonely', 5050)];
    const bins = aggregateToPixels(markers, view, 1000);
    expect(bins).toHaveLength(1);
    expect(bins[0]!.count).toBe(1);
    expect(bins[0]!.representative.id).toBe('lonely');
  });

  it('counts every event landing in the same pixel rather than averaging them', () => {
    const markers = [clip('a', 100), clip('b', 105), clip('c', 110)]; // all within pixel 1 (100..200)
    const bins = aggregateToPixels(markers, view, 1000);
    expect(bins).toHaveLength(1);
    expect(bins[0]!.count).toBe(3);
  });

  it('keeps the highest-severity marker as the representative, not the first one', () => {
    const markers = [clip('a', 100, 'info'), clip('b', 110, 'error'), clip('c', 120, 'warning')];
    const bins = aggregateToPixels(markers, view, 1000);
    expect(bins[0]!.representative.id).toBe('b');
  });

  it('drops markers outside the pixel width', () => {
    const markers = [clip('offscreen', -500), clip('onscreen', 100), clip('far', 1_000_000)];
    const bins = aggregateToPixels(markers, view, 1000);
    expect(bins.map((b) => b.representative.id)).toEqual(['onscreen']);
  });
});

describe('thinToPixels', () => {
  it('always keeps the first marker even under aggressive thinning', () => {
    const markers = Array.from({ length: 20 }, (_, i) => clip(`m${i}`, i * 10));
    const out = thinToPixels(markers, view, 50);
    expect(out[0]!.id).toBe('m0');
  });

  it('respects minimum pixel spacing', () => {
    const markers = Array.from({ length: 20 }, (_, i) => clip(`m${i}`, i * 100)); // 1px apart
    const out = thinToPixels(markers, view, 5);
    for (let i = 1; i < out.length; i++) {
      const dx = (out[i]!.startFrame - out[i - 1]!.startFrame) / view.framesPerPixel;
      expect(dx).toBeGreaterThanOrEqual(5);
    }
  });
});

describe('regionsToPixelSpans', () => {
  it('merges adjacent/overlapping regions into one span', () => {
    const markers: Marker[] = [
      { id: 'a', kind: 'silence', startFrame: 0, endFrame: 1000 },
      { id: 'b', kind: 'silence', startFrame: 1000, endFrame: 2000 }, // touches a
    ];
    const spans = regionsToPixelSpans(markers, view, 1000);
    expect(spans).toHaveLength(1);
    expect(spans[0]!.markers).toHaveLength(2);
  });

  it('keeps distant regions separate', () => {
    const markers: Marker[] = [
      { id: 'a', kind: 'silence', startFrame: 0, endFrame: 100 },
      { id: 'b', kind: 'silence', startFrame: 50_000, endFrame: 50_100 },
    ];
    const spans = regionsToPixelSpans(markers, view, 1000);
    expect(spans).toHaveLength(2);
  });
});

describe('resampleCurveToPixels', () => {
  it('keeps min/max envelope rather than a single sample, so a spike inside a pixel survives', () => {
    const series: CurveSeries = {
      kind: 'loudness',
      framesPerSample: 10,
      startFrame: 0,
      values: Float32Array.from([0, 0, 0, 100, 0, 0, 0, 0, 0, 0]), // spike at sample 3
      minValue: 0,
      maxValue: 100,
    };
    // Each pixel spans framesPerPixel=100 frames = 10 samples, so all 10 samples land in pixel 0.
    const out = resampleCurveToPixels(series, view, 4);
    expect(out[0]!.min).toBe(0);
    expect(out[0]!.max).toBe(100);
  });

  it('returns nothing for an empty series', () => {
    const series: CurveSeries = { kind: 'loudness', framesPerSample: 10, startFrame: 0, values: new Float32Array(0), minValue: 0, maxValue: 1 };
    expect(resampleCurveToPixels(series, view, 100)).toEqual([]);
  });
});
