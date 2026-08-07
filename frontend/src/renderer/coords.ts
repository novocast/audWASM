// The *only* frame<->pixel<->time conversions in the renderer (M17 "Decision — framesPerPixel is
// the zoom primitive"). Every other module must go through these functions rather than computing
// its own — that discipline is the whole mitigation for coordinate-bug drift.
//
// Pixels here are CSS pixels (device-pixel scaling is a separate multiply the caller applies once,
// via ViewState.devicePixelRatio, at the point it touches a canvas context).

export interface FrameToPixelParams {
  startFrame: number;
  framesPerPixel: number;
}

/** Fractional source frame -> fractional CSS-pixel x-coordinate. Inverse of pixelToFrame. */
export function frameToPixel(frame: number, view: FrameToPixelParams): number {
  return (frame - view.startFrame) / view.framesPerPixel;
}

/** CSS-pixel x-coordinate -> fractional source frame. Inverse of frameToPixel. */
export function pixelToFrame(pixel: number, view: FrameToPixelParams): number {
  return view.startFrame + pixel * view.framesPerPixel;
}

export function frameToSeconds(frame: number, sampleRate: number): number {
  return frame / sampleRate;
}

export function secondsToFrame(seconds: number, sampleRate: number): number {
  return seconds * sampleRate;
}

export function pixelToSeconds(pixel: number, view: FrameToPixelParams, sampleRate: number): number {
  return frameToSeconds(pixelToFrame(pixel, view), sampleRate);
}

export function secondsToPixel(seconds: number, view: FrameToPixelParams, sampleRate: number): number {
  return frameToPixel(secondsToFrame(seconds, sampleRate), view);
}

/** How many CSS pixels wide `frameCount` frames span at the current zoom. */
export function frameSpanToPixels(frameCount: number, view: FrameToPixelParams): number {
  return frameCount / view.framesPerPixel;
}

export function pixelSpanToFrames(pixelCount: number, view: FrameToPixelParams): number {
  return pixelCount * view.framesPerPixel;
}

/**
 * Zoom anchored at a fixed pixel (M17 "Zoom anchoring"): the source frame under `anchorPixel`
 * stays under it after the zoom. `factor` > 1 zooms out (more frames/pixel), < 1 zooms in.
 * Returns the new `startFrame`; the caller is responsible for clamping and for computing
 * `newFramesPerPixel` (kept as an explicit input rather than recomputed here, since callers
 * usually need to clamp framesPerPixel to view-model limits before the anchor math applies).
 */
export function zoomAnchoredStartFrame(
  anchorPixel: number,
  view: FrameToPixelParams,
  newFramesPerPixel: number,
): number {
  const frameUnderAnchor = pixelToFrame(anchorPixel, view);
  return frameUnderAnchor - anchorPixel * newFramesPerPixel;
}

export function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

/**
 * Device-pixel-accurate stroke offset for crisp 1px lines (M17 "Crisp rendering"): odd-width
 * strokes must be centred on a device-pixel boundary, not straddle one. Pass a device-pixel
 * coordinate; returns the coordinate to draw at so a 1-device-pixel-wide stroke is sharp.
 */
export function crispStrokeCoord(devicePixelCoord: number): number {
  return Math.round(devicePixelCoord) + 0.5;
}

/** CSS pixel -> device pixel, and back. Every canvas draw call must be in device pixels
 *  (M17 "Device pixel ratio"); these are the only two functions that scale by DPR. */
export function cssToDevicePixel(cssPixel: number, dpr: number): number {
  return cssPixel * dpr;
}

export function deviceToCssPixel(devicePixel: number, dpr: number): number {
  return devicePixel / dpr;
}

// --- Frequency axis (M07 "Frequency axis") --------------------------------------------------
//
// The missing y-axis conversion pair: none existed before M07 (the waveform has no frequency
// dimension). Mirrors engine/spectrogram/freq_mapping.cpp's scale formulas exactly — Log/Mel/Bark
// ruler ticks need to land on the same Hz the tile pixels actually represent, or the ruler and the
// spectrogram image visibly disagree. Linear/Log/Mel have closed-form inverses; Bark doesn't (a sum
// of two arctangents), so its inverse is bisected, same as the C++ side — cheap, only ever called
// for a ruler's ~10 ticks per redraw, never per pixel/per frame.

export type FreqAxis = 'linear' | 'log' | 'mel' | 'bark';

export interface FreqToPixelParams {
  axis: FreqAxis;
  /** Ignored for 'linear' (which always starts at 0 Hz). */
  minHz: number;
  maxHz: number; // typically the Nyquist frequency
  /** Pixel span the axis is drawn over. Convention: pixel 0 = maxHz (top of a spectrogram), pixel
   *  heightCss = the axis's low end — matches the visual "high frequencies at the top" convention. */
  heightCss: number;
}

function hzToScale(axis: FreqAxis, hz: number): number {
  switch (axis) {
    case 'linear':
      return hz;
    case 'log':
      return Math.log2(Math.max(hz, 1e-6));
    case 'mel':
      return 2595 * Math.log10(1 + hz / 700);
    case 'bark':
      return 13 * Math.atan(0.00076 * hz) + 3.5 * Math.atan((hz / 7500) * (hz / 7500));
  }
}

function scaleToHz(axis: FreqAxis, scale: number, nyquistHz: number): number {
  switch (axis) {
    case 'linear':
      return scale;
    case 'log':
      return Math.pow(2, scale);
    case 'mel':
      return 700 * (Math.pow(10, scale / 2595) - 1);
    case 'bark': {
      let lo = 0;
      let hi = nyquistHz;
      for (let i = 0; i < 40; i++) {
        const mid = 0.5 * (lo + hi);
        if (hzToScale('bark', mid) < scale) lo = mid;
        else hi = mid;
      }
      return 0.5 * (lo + hi);
    }
  }
}

function scaleBounds(params: FreqToPixelParams): { sLo: number; sHi: number } {
  const loHz = params.axis === 'linear' ? 0 : params.minHz;
  return { sLo: hzToScale(params.axis, loHz), sHi: hzToScale(params.axis, params.maxHz) };
}

/** Hz -> CSS-pixel y-coordinate (pixel 0 = maxHz, at the top). Inverse of pixelToHz. */
export function hzToPixel(hz: number, params: FreqToPixelParams): number {
  const { sLo, sHi } = scaleBounds(params);
  const frac = (hzToScale(params.axis, hz) - sLo) / (sHi - sLo);
  return (1 - frac) * params.heightCss;
}

/** CSS-pixel y-coordinate -> Hz (pixel 0 = maxHz, at the top). Inverse of hzToPixel. */
export function pixelToHz(pixel: number, params: FreqToPixelParams): number {
  const { sLo, sHi } = scaleBounds(params);
  const frac = params.heightCss <= 0 ? 0 : 1 - pixel / params.heightCss;
  return scaleToHz(params.axis, sLo + frac * (sHi - sLo), params.maxHz);
}
