import { describe, expect, it } from 'vitest';
import { layoutChannelBands } from './renderer.ts';

describe('layoutChannelBands', () => {
  it("tiles the full height exactly for an odd 'split' stereo height (fractional-boundary case)", () => {
    // 221 / 2 = 110.5 — the exact "sub-pixel polish" gap this follow-up fixes: naive division
    // leaves the shared boundary at y=110.5 in both backends. Every boundary here must be an
    // integer device pixel, and the two bands must still tile 221px with no gap/overlap.
    const bands = layoutChannelBands('split', 2, 221);
    expect(bands).toHaveLength(2);
    for (const band of bands) {
      expect(Number.isInteger(band.topDevicePx)).toBe(true);
      expect(Number.isInteger(band.heightDevicePx)).toBe(true);
    }
    expect(bands[0]!.topDevicePx).toBe(0);
    expect(bands[1]!.topDevicePx).toBe(bands[0]!.heightDevicePx);
    expect(bands[1]!.topDevicePx + bands[1]!.heightDevicePx).toBe(221);
  });

  it("'overlaid' packs every channel into a single full-height band", () => {
    const bands = layoutChannelBands('overlaid', 2, 300);
    expect(bands).toEqual([
      { channelIndex: 0, topDevicePx: 0, heightDevicePx: 300 },
      { channelIndex: 1, topDevicePx: 0, heightDevicePx: 300 },
    ]);
  });

  it("'monoSum' produces one full-height band", () => {
    const bands = layoutChannelBands('monoSum', 2, 300);
    expect(bands).toEqual([{ channelIndex: 0, topDevicePx: 0, heightDevicePx: 300 }]);
  });

  it("'midSide' produces two equal-ish bands tiling the full height, even at odd heights", () => {
    const bands = layoutChannelBands('midSide', 2, 151);
    expect(bands).toHaveLength(2);
    expect(bands[0]!.topDevicePx).toBe(0);
    expect(bands[1]!.topDevicePx + bands[1]!.heightDevicePx).toBe(151);
  });

  it("'split' with an odd (>2) channel count still tiles the full height with integer boundaries", () => {
    const bands = layoutChannelBands('split', 3, 100);
    expect(bands).toHaveLength(3);
    let cursor = 0;
    for (const band of bands) {
      expect(band.topDevicePx).toBe(cursor);
      expect(Number.isInteger(band.heightDevicePx)).toBe(true);
      cursor += band.heightDevicePx;
    }
    expect(cursor).toBe(100);
  });
});
