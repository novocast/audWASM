import { describe, expect, it } from 'vitest';
import {
  clamp,
  crispStrokeCoord,
  cssToDevicePixel,
  deviceToCssPixel,
  frameSpanToPixels,
  frameToPixel,
  frameToSeconds,
  hzToPixel,
  pixelSpanToFrames,
  pixelToFrame,
  pixelToHz,
  secondsToFrame,
  zoomAnchoredStartFrame,
  type FreqAxis,
  type FrameToPixelParams,
  type FreqToPixelParams,
} from './coords.ts';

describe('frameToPixel / pixelToFrame round-trip', () => {
  it('round-trips for ordinary zoom levels', () => {
    const view: FrameToPixelParams = { startFrame: 12345.5, framesPerPixel: 3.25 };
    for (const pixel of [0, 1, 42.7, 999.999, -5]) {
      const frame = pixelToFrame(pixel, view);
      expect(frameToPixel(frame, view)).toBeCloseTo(pixel, 9);
    }
  });

  it('round-trips at the "1 frame across the full width" extreme (framesPerPixel << 1)', () => {
    const view: FrameToPixelParams = { startFrame: 1_000_000, framesPerPixel: 1 / 800 };
    for (const pixel of [0, 400, 799.5]) {
      const frame = pixelToFrame(pixel, view);
      expect(frameToPixel(frame, view)).toBeCloseTo(pixel, 6);
    }
  });

  it('round-trips at the "3-hour file in 800px" extreme (framesPerPixel very large)', () => {
    const totalFrames = 3 * 3600 * 44100; // 3 hours @ 44.1kHz
    const framesPerPixel = totalFrames / 800;
    const view: FrameToPixelParams = { startFrame: 0, framesPerPixel };
    for (const pixel of [0, 1, 400, 799]) {
      const frame = pixelToFrame(pixel, view);
      expect(frameToPixel(frame, view)).toBeCloseTo(pixel, 3);
    }
  });

  it('round-trips with a fractional startFrame', () => {
    const view: FrameToPixelParams = { startFrame: 0.5, framesPerPixel: 0.1 };
    const frame = pixelToFrame(10, view);
    expect(frame).toBeCloseTo(1.5, 9);
    expect(frameToPixel(frame, view)).toBeCloseTo(10, 9);
  });
});

describe('zoom anchoring invariance', () => {
  it('keeps the frame under the anchor pixel fixed across a zoom-in', () => {
    const view: FrameToPixelParams = { startFrame: 500, framesPerPixel: 4 };
    const anchorPixel = 150;
    const frameUnderAnchorBefore = pixelToFrame(anchorPixel, view);

    const newFramesPerPixel = view.framesPerPixel / 3;
    const newStartFrame = zoomAnchoredStartFrame(anchorPixel, view, newFramesPerPixel);
    const after: FrameToPixelParams = { startFrame: newStartFrame, framesPerPixel: newFramesPerPixel };

    expect(pixelToFrame(anchorPixel, after)).toBeCloseTo(frameUnderAnchorBefore, 9);
  });

  it('keeps the frame under the anchor pixel fixed across a zoom-out', () => {
    const view: FrameToPixelParams = { startFrame: 10_000, framesPerPixel: 0.5 };
    const anchorPixel = 723.3;
    const frameUnderAnchorBefore = pixelToFrame(anchorPixel, view);

    const newFramesPerPixel = view.framesPerPixel * 12;
    const newStartFrame = zoomAnchoredStartFrame(anchorPixel, view, newFramesPerPixel);
    const after: FrameToPixelParams = { startFrame: newStartFrame, framesPerPixel: newFramesPerPixel };

    expect(pixelToFrame(anchorPixel, after)).toBeCloseTo(frameUnderAnchorBefore, 6);
  });

  it('is invariant under repeated zoom in/out cycles (no drift)', () => {
    let view: FrameToPixelParams = { startFrame: 8675, framesPerPixel: 2 };
    const anchorPixel = 200;
    const frameUnderAnchorBefore = pixelToFrame(anchorPixel, view);

    for (let i = 0; i < 20; i++) {
      const factor = i % 2 === 0 ? 1.37 : 1 / 1.37;
      const newFramesPerPixel = view.framesPerPixel * factor;
      const newStartFrame = zoomAnchoredStartFrame(anchorPixel, view, newFramesPerPixel);
      view = { startFrame: newStartFrame, framesPerPixel: newFramesPerPixel };
    }

    expect(pixelToFrame(anchorPixel, view)).toBeCloseTo(frameUnderAnchorBefore, 3);
  });

  it('holds at the single-sample-across-full-width extreme', () => {
    const view: FrameToPixelParams = { startFrame: 42, framesPerPixel: 1 / 2000 };
    const anchorPixel = 999;
    const frameUnderAnchorBefore = pixelToFrame(anchorPixel, view);
    const newFramesPerPixel = view.framesPerPixel * 5;
    const newStartFrame = zoomAnchoredStartFrame(anchorPixel, view, newFramesPerPixel);
    const after: FrameToPixelParams = { startFrame: newStartFrame, framesPerPixel: newFramesPerPixel };
    expect(pixelToFrame(anchorPixel, after)).toBeCloseTo(frameUnderAnchorBefore, 6);
  });
});

describe('frame/second conversions', () => {
  it('are inverse operations at a typical sample rate', () => {
    const sampleRate = 48000;
    for (const seconds of [0, 0.001, 1.5, 3600]) {
      expect(frameToSeconds(secondsToFrame(seconds, sampleRate), sampleRate)).toBeCloseTo(seconds, 9);
    }
  });
});

describe('pixel span helpers', () => {
  it('frameSpanToPixels and pixelSpanToFrames are inverse', () => {
    const view: FrameToPixelParams = { startFrame: 0, framesPerPixel: 7.3 };
    const frames = 512;
    const pixels = frameSpanToPixels(frames, view);
    expect(pixelSpanToFrames(pixels, view)).toBeCloseTo(frames, 9);
  });
});

describe('clamp', () => {
  it('clamps into range and passes through in-range values', () => {
    expect(clamp(5, 0, 10)).toBe(5);
    expect(clamp(-5, 0, 10)).toBe(0);
    expect(clamp(50, 0, 10)).toBe(10);
  });
});

describe('device pixel helpers', () => {
  it('cssToDevicePixel/deviceToCssPixel round-trip at several DPRs', () => {
    for (const dpr of [1, 1.5, 2, 3]) {
      const css = 123.456;
      expect(deviceToCssPixel(cssToDevicePixel(css, dpr), dpr)).toBeCloseTo(css, 9);
    }
  });

  it('crispStrokeCoord centres a 1px stroke on a device-pixel boundary', () => {
    expect(crispStrokeCoord(10.2)).toBe(10.5);
    expect(crispStrokeCoord(10.7)).toBe(11.5);
    expect(Number.isInteger(crispStrokeCoord(10) - 0.5)).toBe(true);
  });
});

describe('hzToPixel / pixelToHz (M07 frequency axis)', () => {
  const axes: FreqAxis[] = ['linear', 'log', 'mel', 'bark'];

  it('round-trips for every axis', () => {
    for (const axis of axes) {
      const params: FreqToPixelParams = { axis, minHz: 20, maxHz: 20000, heightCss: 256 };
      for (const hz of [20, 100, 440, 1000, 5000, 19999]) {
        const pixel = hzToPixel(hz, params);
        expect(pixelToHz(pixel, params)).toBeCloseTo(hz, 0);
      }
    }
  });

  it('pixel 0 is the top (maxHz) and pixel heightCss is the bottom (minHz), for every axis', () => {
    for (const axis of axes) {
      const params: FreqToPixelParams = { axis, minHz: 20, maxHz: 20000, heightCss: 256 };
      expect(hzToPixel(20000, params)).toBeCloseTo(0, 3);
      expect(hzToPixel(axis === 'linear' ? 0 : 20, params)).toBeCloseTo(256, 3);
    }
  });

  it('log axis compresses high frequencies more than low ones (log-curve shape)', () => {
    const params: FreqToPixelParams = { axis: 'log', minHz: 20, maxHz: 20000, heightCss: 256 };
    const lowSpanPx = hzToPixel(20, params) - hzToPixel(40, params); // one octave near the bottom
    const highSpanPx = hzToPixel(10000, params) - hzToPixel(20000, params); // one octave near the top
    expect(lowSpanPx).toBeCloseTo(highSpanPx, 1); // log axis: equal octaves get equal pixel spans...
    const linearParams: FreqToPixelParams = { axis: 'linear', minHz: 0, maxHz: 20000, heightCss: 256 };
    const linearLowSpan = hzToPixel(20, linearParams) - hzToPixel(40, linearParams);
    const linearHighSpan = hzToPixel(10000, linearParams) - hzToPixel(20000, linearParams);
    expect(linearHighSpan).toBeGreaterThan(linearLowSpan * 50); // ...unlike a linear axis
  });
});
