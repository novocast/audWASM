// Waveform draw for the Canvas2D backend, all three zoom regimes (M17 "Waveform rendering").
// One Path2D per redraw (M17: "batching into one path rather than per-bar strokes is ~10x
// faster") rather than one fillRect/stroke call per bin.
//
// The regime only changes *draw style*; the actual bin data always comes from the single
// `RenderFrame.waveform` query the app loop already fetched at devicePixelWidth bins (M17
// "Decision — engine data queries happen synchronously inside the frame"). Below level 0, the
// engine's raw-PCM fallback (M05) means min===max per bin when there's exactly one frame per
// device pixel, which is what makes the 'sample' polyline below correct without a separate
// sample-fetch API.
//
// Band-limited (Lanczos-windowed sinc) reconstruction for the 'point' regime lives at the bottom
// of this file (buildSincCurve()/extractPointSamples()) — see M17 "Known follow-ups": it draws a
// real interpolation curve between actual samples now, not just a straight-line polyline, so
// inter-sample overshoot (the M11 tie-in) is visible at extreme zoom. It's computed purely for
// display from the bins already queried this frame — it does not call an engine true-peak/ISP API,
// because M08/M11 (the analytical true-peak pipeline this curve visually echoes) haven't been
// built yet; see the M17 doc's "Known follow-ups" for the distinction.

import {
  amplitudeToUnit,
  layoutChannelBands,
  waveformQueryBinCount,
  type ChannelBand,
  type RenderFrame,
  type WaveformBinsLike,
} from '../renderer.ts';
import type { ThemeTokens } from '../theme.ts';
import { crispStrokeCoord } from '../coords.ts';

export type WaveformRegime = 'summary' | 'sample' | 'point';

const kSampleRegimeMaxFpp = 1;
const kPointRegimeMaxFpp = 1 / 16;

export function waveformRegimeFor(framesPerPixel: number): WaveformRegime {
  if (framesPerPixel < kPointRegimeMaxFpp) return 'point';
  if (framesPerPixel <= kSampleRegimeMaxFpp) return 'sample';
  return 'summary';
}

type BandLayout = ChannelBand;

function layoutBands(frame: RenderFrame, devicePixelHeight: number): BandLayout[] {
  const trackChannels = Math.max(1, frame.waveform?.channels ?? 1);
  return layoutChannelBands(frame.view.channelLayout, trackChannels, devicePixelHeight);
}

/** Thin divider stroke at every internal band boundary (M17 follow-up "sub-pixel polish") — purely
 *  a visual separator between e.g. the two 'split' stereo bands or the mid/side pair; drawn once
 *  per redraw, not per layer, since it never depends on the waveform regime. */
function drawBandDividers(ctx: CanvasRenderingContext2D, bands: BandLayout[], devicePixelWidth: number, theme: ThemeTokens): void {
  if (bands.length < 2) return;
  const seen = new Set<number>();
  ctx.strokeStyle = cssRgba(theme.grid);
  ctx.lineWidth = 1;
  const path = new Path2D();
  for (const band of bands) {
    const boundary = crispStrokeCoord(band.topDevicePx);
    if (band.topDevicePx <= 0 || seen.has(boundary)) continue;
    seen.add(boundary);
    path.moveTo(0, boundary);
    path.lineTo(devicePixelWidth, boundary);
  }
  ctx.stroke(path);
}

function drawSummaryBand(
  ctx: CanvasRenderingContext2D,
  bins: WaveformBinsLike,
  binOffset: number,
  binCount: number,
  band: BandLayout,
  frame: RenderFrame,
  theme: ThemeTokens,
): void {
  const midY = band.topDevicePx + band.heightDevicePx / 2;
  const scale = (band.heightDevicePx / 2) * frame.view.verticalZoom;
  const minMaxPath = new Path2D();
  const rmsPath = new Path2D();

  for (let x = 0; x < binCount; x++) {
    const i = binOffset + x;
    const maxUnit = amplitudeToUnit(bins.max(i), frame.view.amplitudeScale);
    const minUnit = amplitudeToUnit(bins.min(i), frame.view.amplitudeScale);
    const rms = amplitudeToUnit(bins.rms(i), frame.view.amplitudeScale);
    const yTop = midY - maxUnit * scale;
    const yBottom = midY - minUnit * scale;
    minMaxPath.rect(x, Math.min(yTop, yBottom), 1, Math.max(1, Math.abs(yBottom - yTop)));
    const rmsHalf = Math.abs(rms) * scale;
    rmsPath.rect(x, midY - rmsHalf, 1, Math.max(1, rmsHalf * 2));
  }

  ctx.fillStyle = cssRgba(theme.waveformFill);
  ctx.fill(minMaxPath);
  ctx.fillStyle = cssRgba(theme.waveformRms);
  ctx.fill(rmsPath);
}

function drawSampleBand(
  ctx: CanvasRenderingContext2D,
  bins: WaveformBinsLike,
  binOffset: number,
  binCount: number,
  band: BandLayout,
  frame: RenderFrame,
  theme: ThemeTokens,
  isRawPcm: boolean,
): void {
  const midY = band.topDevicePx + band.heightDevicePx / 2;
  const scale = (band.heightDevicePx / 2) * frame.view.verticalZoom;
  const path = new Path2D();
  for (let x = 0; x < binCount; x++) {
    const i = binOffset + x;
    // min===max for a true 1-frame-per-bin raw sample; averaging is a harmless, cheap fallback
    // if the engine ever returns >1 frame/bin here (e.g. right at the sample/summary boundary).
    const value = amplitudeToUnit((bins.min(i) + bins.max(i)) / 2, frame.view.amplitudeScale);
    const y = crispStrokeCoord(midY - value * scale);
    if (x === 0) path.moveTo(x + 0.5, y);
    else path.lineTo(x + 0.5, y);
  }
  ctx.strokeStyle = cssRgba(isRawPcm ? theme.waveformRawPcm : theme.waveformFill);
  ctx.lineWidth = 1;
  ctx.stroke(path);
}

interface PointSample {
  /** Pixel x at the centre of this sample's run of identical bins (device pixels). */
  pixelX: number;
  /** Raw (pre-amplitude-scale) sample value, so interpolation happens in linear PCM space. */
  value: number;
}

/**
 * At the 'point' regime the engine queries one bin per device pixel, so a single source frame
 * spans many consecutive bins with an identical min===max value (M05's raw-PCM fallback repeated
 * across every pixel that frame covers). Collapse each such run to one `PointSample` at its
 * midpoint pixel — that recovers the actual per-frame sample positions from the per-pixel bin
 * buffer without a separate sample-fetch API.
 */
export function extractPointSamples(bins: WaveformBinsLike, binOffset: number, binCount: number): PointSample[] {
  const samples: PointSample[] = [];
  let runStart = 0;
  let runValue = binCount > 0 ? (bins.min(binOffset) + bins.max(binOffset)) / 2 : 0;
  for (let x = 1; x <= binCount; x++) {
    const value = x < binCount ? (bins.min(binOffset + x) + bins.max(binOffset + x)) / 2 : NaN;
    if (x === binCount || value !== runValue) {
      samples.push({ pixelX: (runStart + x - 1) / 2, value: runValue });
      runStart = x;
      runValue = value;
    }
  }
  return samples;
}

function sinc(x: number): number {
  if (x === 0) return 1;
  const px = Math.PI * x;
  return Math.sin(px) / px;
}

/** Lanczos-windowed sinc kernel: the ideal (infinite-support) `sinc(x)` reconstruction filter
 *  multiplied by a `sinc(x/a)` taper that reaches zero at `|x| = a`, giving a cheap finite-tap
 *  kernel that still reproduces every sample exactly (`sinc(0) = 1`, `sinc(nonzero int) = 0`)
 *  while keeping the reconstruction's signature overshoot near sharp transitions. */
function lanczos(x: number, a: number): number {
  if (x <= -a || x >= a) return 0;
  return sinc(x) * sinc(x / a);
}

const kLanczosTaps = 4;

/**
 * Band-limited reconstruction of `samples` at every pixel in `[0, binCount)`, in raw PCM space
 * (M17 "the reconstruction curve ... ties directly to M11's inter-sample peaks" — the whole point
 * is to show overshoot between samples, which is exactly what a straight-line polyline cannot show
 * and windowed-sinc interpolation reproduces by construction). `pixelsPerFrame` is the sample
 * spacing in device pixels (`1 / framesPerPixel`); only samples within `kLanczosTaps` frames of a
 * given pixel contribute, so this stays linear in `binCount` rather than quadratic.
 */
export function buildSincCurve(samples: PointSample[], pixelsPerFrame: number, binCount: number): Float64Array {
  const out = new Float64Array(binCount);
  if (samples.length === 0 || pixelsPerFrame <= 0) return out;
  const windowPx = kLanczosTaps * pixelsPerFrame;
  let lo = 0;
  for (let x = 0; x < binCount; x++) {
    while (lo + 1 < samples.length && samples[lo + 1]!.pixelX < x - windowPx) lo++;
    let sum = 0;
    for (let s = lo; s < samples.length && samples[s]!.pixelX <= x + windowPx; s++) {
      const sample = samples[s]!;
      const d = (x - sample.pixelX) / pixelsPerFrame;
      sum += sample.value * lanczos(d, kLanczosTaps);
    }
    out[x] = sum;
  }
  return out;
}

function drawPointBand(
  ctx: CanvasRenderingContext2D,
  bins: WaveformBinsLike,
  binOffset: number,
  binCount: number,
  band: BandLayout,
  frame: RenderFrame,
  theme: ThemeTokens,
): void {
  const midY = band.topDevicePx + band.heightDevicePx / 2;
  const scale = (band.heightDevicePx / 2) * frame.view.verticalZoom;
  const kDotRadius = 3;

  const samples = extractPointSamples(bins, binOffset, binCount);
  const pixelsPerFrame = Math.max(1 / Math.max(frame.view.framesPerPixel, 1e-9), 1);
  const curve = buildSincCurve(samples, pixelsPerFrame, binCount);

  const curvePath = new Path2D();
  for (let x = 0; x < binCount; x++) {
    const y = midY - amplitudeToUnit(curve[x]!, frame.view.amplitudeScale) * scale;
    if (x === 0) curvePath.moveTo(x + 0.5, y);
    else curvePath.lineTo(x + 0.5, y);
  }

  const dotPath = new Path2D();
  for (const s of samples) {
    const y = midY - amplitudeToUnit(s.value, frame.view.amplitudeScale) * scale;
    dotPath.moveTo(s.pixelX + 0.5 + kDotRadius, y);
    dotPath.arc(s.pixelX + 0.5, y, kDotRadius, 0, Math.PI * 2);
  }

  ctx.strokeStyle = cssRgba({ ...theme.waveformFill, a: theme.waveformFill.a * 0.7 });
  ctx.lineWidth = 1;
  ctx.stroke(curvePath);
  ctx.fillStyle = cssRgba(theme.waveformFill);
  ctx.fill(dotPath);
}

export function drawWaveformLayer(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
): void {
  if (!frame.waveform) return;
  const regime = waveformRegimeFor(frame.view.framesPerPixel);
  const bands = layoutBands(frame, devicePixelHeight);
  const { bins } = frame.waveform;
  const binCount = waveformQueryBinCount(frame.waveform);

  for (const band of bands) {
    const binOffset = band.channelIndex * binCount;
    switch (regime) {
      case 'summary':
        drawSummaryBand(ctx, bins, binOffset, Math.min(binCount, devicePixelWidth), band, frame, frame.theme);
        break;
      case 'sample':
        drawSampleBand(ctx, bins, binOffset, Math.min(binCount, devicePixelWidth), band, frame, frame.theme, frame.waveform.isRawPcm);
        break;
      case 'point':
        drawPointBand(ctx, bins, binOffset, Math.min(binCount, devicePixelWidth), band, frame, frame.theme);
        break;
    }
  }
  drawBandDividers(ctx, bands, devicePixelWidth, frame.theme);
}

function cssRgba(c: { r: number; g: number; b: number; a: number }): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}
