// M18's density strategies (task doc's "The real problem is density, not drawing" table): one
// implementation per strategy, each taking an already-visible marker slice (from store.ts) plus
// the current view and producing pixel-resolution draw primitives — never re-touching markers
// outside the visible window, and never doing per-marker work once a pixel bucket already has one.
//
// "Aggregate must use max/count, not mean" (M05/M07's principle, repeated here because it's the
// difference between a useful overlay and one that quietly hides a single clipping event).

import { frameToPixel, type FrameToPixelParams } from '../renderer/coords.ts';
import type { CurveSeries, Marker } from './model.ts';

export interface AggregateBin {
  pixel: number;
  count: number;
  /** The marker "worth showing" for this bin if only one can be — the highest-priority-severity
   *  one, falling back to the first. Never a synthetic average; always a real marker. */
  representative: Marker;
}

/** Bins visible markers one-per-device-pixel-column and keeps a count + a representative marker
 *  per bin, never averaging. A single event landing alone in its pixel is bin `{count: 1, ...}` —
 *  indistinguishable in the data from "aggregated" beyond the count, which is exactly the "don't
 *  lose the needle" acceptance criterion: intensity/count scales with density, but one real event
 *  is always the thing drawn. */
export function aggregateToPixels(markers: readonly Marker[], view: FrameToPixelParams, widthPx: number): AggregateBin[] {
  const bins = new Map<number, AggregateBin>();
  for (const m of markers) {
    const px = Math.floor(frameToPixel(m.startFrame, view));
    if (px < 0 || px >= widthPx) continue;
    const existing = bins.get(px);
    if (!existing) {
      bins.set(px, { pixel: px, count: 1, representative: m });
      continue;
    }
    existing.count++;
    if (severityRank(m.severity) > severityRank(existing.representative.severity)) {
      existing.representative = m;
    }
  }
  return [...bins.values()].sort((a, b) => a.pixel - b.pixel);
}

function severityRank(s: Marker['severity']): number {
  switch (s) {
    case 'error':
      return 3;
    case 'warning':
      return 2;
    case 'info':
      return 1;
    default:
      return 0;
  }
}

/** Thins a dense point kind (beats) by minimum pixel spacing as zoom decreases — "draw every Nth
 *  (bars instead of beats)". Always keeps the first visible marker so a run of thinned-out markers
 *  never silently starts mid-sequence with no visual anchor. */
export function thinToPixels(markers: readonly Marker[], view: FrameToPixelParams, minSpacingPx: number): Marker[] {
  const out: Marker[] = [];
  let lastPx = -Infinity;
  for (const m of markers) {
    const px = frameToPixel(m.startFrame, view);
    if (px - lastPx >= minSpacingPx) {
      out.push(m);
      lastPx = px;
    }
  }
  return out;
}

export interface RegionSpan {
  startPx: number;
  endPx: number;
  markers: Marker[];
}

/**
 * Merges region markers (silence, selections, chapters, dcRegion, loopRegion) that overlap or are
 * within `mergeToleranceMs`-worth of pixels of each other into visually contiguous spans — "regions
 * merge visually when adjacent; always drawn" (never thinned/aggregated away, unlike point kinds).
 * Point markers (no `endFrame`) are treated as a single-pixel-wide span.
 */
export function regionsToPixelSpans(markers: readonly Marker[], view: FrameToPixelParams, widthPx: number, mergeTolerancePx = 1): RegionSpan[] {
  const spans: RegionSpan[] = [];
  for (const m of markers) {
    const rawStart = frameToPixel(m.startFrame, view);
    const rawEnd = m.endFrame !== undefined ? frameToPixel(m.endFrame, view) : rawStart;
    const startPx = Math.max(0, Math.min(rawStart, rawEnd));
    const endPx = Math.min(widthPx, Math.max(rawStart, rawEnd, startPx));
    if (endPx < 0 || startPx > widthPx) continue;

    const last = spans.at(-1);
    if (last && startPx <= last.endPx + mergeTolerancePx) {
      last.endPx = Math.max(last.endPx, endPx);
      last.markers.push(m);
    } else {
      spans.push({ startPx, endPx, markers: [m] });
    }
  }
  return spans;
}

export interface CurvePixelSample {
  pixel: number;
  min: number;
  max: number;
}

/** Resamples a curve series (loudness/RMS/correlation/ODF/DC) to one min/max envelope pair per
 *  device-pixel column — same technique as M05's waveform pyramid, applied to a scalar series
 *  instead of PCM. A pixel wider than one sample keeps both extremes rather than the last value,
 *  so a brief loudness spike inside a pixel is never smoothed away. */
export function resampleCurveToPixels(series: CurveSeries, view: FrameToPixelParams, widthPx: number): CurvePixelSample[] {
  if (series.values.length === 0) return [];
  const out: CurvePixelSample[] = [];
  for (let px = 0; px < widthPx; px++) {
    const startFrame = view.startFrame + px * view.framesPerPixel;
    const endFrame = startFrame + view.framesPerPixel;
    const iStart = Math.max(0, Math.floor((startFrame - series.startFrame) / series.framesPerSample));
    const iEnd = Math.min(series.values.length - 1, Math.ceil((endFrame - series.startFrame) / series.framesPerSample));
    if (iEnd < iStart || iStart >= series.values.length || iEnd < 0) continue;

    let min = Infinity;
    let max = -Infinity;
    for (let i = Math.max(0, iStart); i <= iEnd; i++) {
      const v = series.values[i]!;
      if (v < min) min = v;
      if (v > max) max = v;
    }
    if (min <= max) out.push({ pixel: px, min, max });
  }
  return out;
}
