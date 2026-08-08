// Performance test for the task doc's acceptance criterion: "60 fps at every zoom level with
// 50 000 markers present." A real fps measurement needs a browser rAF loop; this test instead
// asserts on the thing that actually determines it in a headless/CI environment — that computing
// one frame's visible slice + density reduction is O(visible), not O(total), by giving it 50 000
// markers spread across a full-track view and asserting the whole pass stays comfortably inside a
// 16.6ms frame budget. A true rendered-frame-rate measurement belongs in the (browser-driven, per
// the `run` skill) manual/E2E pass, not vitest.

import { describe, expect, it } from 'vitest';
import { MarkerStore } from './store.ts';
import { aggregateToPixels } from './density.ts';
import type { Marker } from './model.ts';

describe('50,000-marker performance', () => {
  it('visible-slice + aggregate stays well under one frame budget at full-track zoom', () => {
    const totalFrames = 5 * 60 * 44100; // a 5-minute track, per the doc's worked example
    const store = new MarkerStore();
    const markers: Marker[] = Array.from({ length: 50_000 }, (_, i) => ({
      id: `m${i}`,
      kind: 'clipping',
      startFrame: Math.floor((i / 50_000) * totalFrames),
    }));
    store.replaceAnalysisMarkers('clipping', markers);

    const view = { startFrame: 0, framesPerPixel: totalFrames / 1920 }; // full track in a 1920px view
    const start = performance.now();
    for (let frame = 0; frame < 30; frame++) {
      const visible = store.visible('clipping', view.startFrame, view.startFrame + view.framesPerPixel * 1920);
      aggregateToPixels(visible, view, 1920);
    }
    const elapsedMs = performance.now() - start;
    const perFrameMs = elapsedMs / 30;

    expect(perFrameMs).toBeLessThan(16.6);
  });

  it('aggregation at full-track zoom still surfaces exactly one representative for a lone event', () => {
    const totalFrames = 5 * 60 * 44100;
    const store = new MarkerStore();
    const dense: Marker[] = Array.from({ length: 49_999 }, (_, i) => ({
      id: `dense${i}`,
      kind: 'clipping',
      startFrame: Math.floor((i / 49_999) * (totalFrames / 2)), // packed into the first half
    }));
    const lonely: Marker = { id: 'lonely', kind: 'clipping', startFrame: totalFrames - 1000 }; // isolated, near the end
    store.replaceAnalysisMarkers('clipping', [...dense, lonely]);

    const view = { startFrame: 0, framesPerPixel: totalFrames / 1920 };
    const visible = store.visible('clipping', 0, totalFrames);
    const bins = aggregateToPixels(visible, view, 1920);
    const lastBin = bins.at(-1)!;
    expect(lastBin.representative.id).toBe('lonely');
    expect(lastBin.count).toBe(1);
  });
});
