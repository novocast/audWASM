// The `Renderer` interface (M17 "Backend strategy") and the layer/dirty-tracking model shared by
// both backends. This file is deliberately backend- and WASM-engine-agnostic: it only knows about
// plain data (bins, ranges, tokens), never a canvas context or an Embind handle, so it stays cheap
// to unit test and keeps the WebGPU path (whenever it lands) a new backend rather than a rewrite.

import type { AmplitudeScale, ChannelLayout, ViewLimits, ViewState } from './viewState.ts';
import type { ThemeTokens } from './theme.ts';
import type { CurveKind, CurveSeries, Marker, OverlayKind } from '../overlays/model.ts';
import type { LaneConfig } from '../overlays/lanes.ts';
import type { HitCandidate } from '../overlays/hitTest.ts';

/** Bottom-to-top compositing order (M17 "Layered scene model"). Layers 5-6 always render on the
 *  dedicated Canvas2D overlay (see playheadOverlay.ts); 0-4 render on the main backend surface. */
export const kLayerOrder = [
  'background',
  'spectrogram',
  'waveform',
  'selection',
  'overlays',
  'cursor',
  'interaction',
] as const;

export type Layer = (typeof kLayerOrder)[number];

/** Per-layer dirty flags + a redraw counter per layer, so tests can assert e.g. "the playhead
 *  moving never increments the waveform redraw counter" (M17 acceptance criteria). */
export class DirtyTracker {
  private readonly dirty = new Set<Layer>();
  private readonly redrawCounts = new Map<Layer, number>(kLayerOrder.map((l) => [l, 0]));

  markDirty(layer: Layer): void {
    this.dirty.add(layer);
  }

  markAllDirty(): void {
    for (const layer of kLayerOrder) this.dirty.add(layer);
  }

  isDirty(layer: Layer): boolean {
    return this.dirty.has(layer);
  }

  /** Call exactly once per frame, right before actually redrawing `layer`. Clears the flag and
   *  bumps the counter, so a layer that wasn't dirty is never counted as redrawn. */
  consumeDirty(layer: Layer): boolean {
    if (!this.dirty.has(layer)) return false;
    this.dirty.delete(layer);
    this.redrawCounts.set(layer, (this.redrawCounts.get(layer) ?? 0) + 1);
    return true;
  }

  redrawCount(layer: Layer): number {
    return this.redrawCounts.get(layer) ?? 0;
  }
}

/** Mirrors bindings/wasm/waveformView.ts's WaveformBinsView shape without importing it, so this
 *  module has no WASM dependency. engine.ts's WaveformBinsView satisfies this structurally. */
export interface WaveformBinsLike {
  readonly binCount: number;
  min(i: number): number;
  max(i: number): number;
  rms(i: number): number;
  absPeak(i: number): number;
}

export interface WaveformQueryResult {
  /** `bins.binCount` is `channels * <bins per band>` (one flat buffer for every band) — use
   *  `waveformQueryBinCount()` below to get the per-band count each layer actually wants. */
  bins: WaveformBinsLike;
  /** Number of channel bands packed into `bins` (query()'s `channels`, not the track's raw
   *  channel count — e.g. 1 for monoSum/midSide-summed layouts). */
  channels: number;
  framesPerBin: number;
  isComplete: boolean;
  isRawPcm: boolean;
}

/** Per-band bin count within a WaveformQueryResult — `bins.binCount` is the flat total across all
 *  channel bands, so every layer needs this division rather than repeating it inline. */
export function waveformQueryBinCount(result: WaveformQueryResult): number {
  return result.bins.binCount / Math.max(1, result.channels);
}

export interface SelectionRange {
  startFrame: number;
  endFrame: number;
}

/** Placeholder shape for M18 overlays/markers — M17 only needs enough to reserve layer 4 and
 *  leave a real seam; M18 owns the actual model. Retained (rather than removed) because
 *  interaction.ts's double-click-to-select-between-markers gesture only needs `{id, frame}` and
 *  predates the full `Marker` model — kept decoupled from overlays/model.ts so interaction.ts
 *  doesn't need to import the whole overlays module for two fields. */
export interface MarkerLike {
  id: string;
  frame: number;
  label?: string;
}

/** Minimal structural view of a MarkerStore (overlays/store.ts) — the renderer only ever reads
 *  per-kind arrays, never mutates, so it depends on this rather than the concrete class (same
 *  discipline as `WaveformBinsLike` above: draw code stays testable without constructing a real
 *  store). */
export interface OverlayMarkerSource {
  get(kind: OverlayKind): readonly Marker[];
}

/** What M18's overlays layer needs each frame, on top of `RenderFrame.markers` above. Null until
 *  a track/analysis session exists — the draw layer just clears in that case, same as any other
 *  not-yet-loaded layer. */
export interface OverlayFrameData {
  markers: OverlayMarkerSource;
  lanes: readonly LaneConfig[];
  curves: Partial<Record<CurveKind, CurveSeries>>;
  /** The marker currently selected in the inspector/findings list, if any — drawn with a highlight
   *  ring so clicking a findings-list row visibly connects to its position on the timeline. */
  selectedMarkerId: string | null;
}

/** Identifies one spectrogram tile — mirrors engine/spectrogram/tile.hpp's TileKey minus
 *  configHash (SpectrogramSource tracks its own current config; a stale-config tile simply isn't
 *  resident under a key the source would hand out). */
export interface SpectrogramTileRef {
  level: number;
  tileX: number;
  channel: number;
}

/** Plain pixel data for one tile or the overview strip — never a GPU texture/handle (M17's
 *  backend-agnostic discipline). `bytes` is `width*height` quantised dB bytes, row 0 = lowest
 *  displayed frequency (see engine/spectrogram/tile.hpp). */
export interface SpectrogramTileBytes {
  bytes: Uint8Array;
  width: number;
  height: number;
  floorDb: number;
  ceilDb: number;
  /** The fftSize this tile/overview was generated at — needed to compute its true time span
   *  (hopForLevel/foldFactorForLevel both depend on it), since fftSize is adaptive and can differ
   *  from whatever the view's *current* zoom would pick by the time this tile is drawn. */
  fftSize: number;
}

/** What the spectrogram layer needs each frame, supplied by the client-side tile manager
 *  (frontend/src/renderer/spectrogram/tileManager.ts) — structurally typed here so this module
 *  stays free of any worker/WASM dependency, per the file header's discipline. */
export interface SpectrogramSource {
  readonly configHash: number;
  /** Tiles the current viewport (+ margin) needs, priority-ordered (closest to viewport centre
   *  first) — computed by the tile manager from whatever ViewState it was last given, not by this
   *  interface's caller. */
  visibleTiles(): readonly SpectrogramTileRef[];
  /** Null if not yet resident (still generating, or never requested) — the renderer should fall
   *  back to a coarser level or the overview strip rather than leaving a gap. */
  tileBytes(ref: SpectrogramTileRef): SpectrogramTileBytes | null;
  /** Null until the worker has produced channel `channel`'s eager overview strip. */
  overviewBytes(channel: number): SpectrogramTileBytes | null;
}

export interface RenderFrame {
  view: ViewState;
  limits: ViewLimits;
  sampleRate: number;
  trackDurationFrames: number;
  /** Null until the engine has produced at least one channel of waveform data. */
  waveform: WaveformQueryResult | null;
  /** Null when nothing is playing/loaded yet. */
  playheadFrame: number | null;
  isPlaying: boolean;
  selection: SelectionRange | null;
  markers: readonly MarkerLike[];
  /** Null until M18 overlay wiring is attached (e.g. in an M17-only test harness) — see
   *  OverlayFrameData's own doc comment. */
  overlays: OverlayFrameData | null;
  theme: ThemeTokens;
  /** Null until the spectrogram worker has loaded PCM for the current track (M07). */
  spectrogram: SpectrogramSource | null;
}

export interface RenderStats {
  frameTimeMs: number;
  layersRedrawn: readonly Layer[];
}

/** What M17's "Backend capability detection and selection" task needs to report per backend. */
export interface BackendCapability {
  backend: 'canvas2d' | 'webgl2';
  available: boolean;
  reason?: string;
}

/**
 * A `Renderer` draws layers 0-4 (background through overlays) onto one main surface; layers 5-6
 * (cursor, interaction feedback) always live on the separate small Canvas2D overlay canvas
 * (playheadOverlay.ts) regardless of which Renderer is active — see M17's single most important
 * performance decision. `render()` is called once per rAF frame from the app loop.
 */
export interface Renderer {
  readonly backend: 'canvas2d' | 'webgl2';
  readonly dirty: DirtyTracker;

  /** `container` is an empty, positioned (`position: relative`) element the Renderer owns
   *  exclusively — it creates and manages whatever canvas element(s) it needs inside it. Kept as
   *  a container rather than a single canvas because Canvas2D needs one stacked canvas per layer
   *  (so a dirty waveform layer doesn't force redrawing the ruler on top of it) while WebGL2
   *  needs exactly one. The playhead/interaction overlay canvas is separate and owned by the app
   *  loop (loop.ts), not by the Renderer, per M17's layering decision. */
  attach(container: HTMLElement): void;
  resize(widthCss: number, heightCss: number, devicePixelRatio: number): void;
  setTheme(theme: ThemeTokens): void;
  render(frame: RenderFrame): RenderStats;
  /** Hit-test candidates built by the most recent render()'s overlays-layer pass (M18) — both
   *  backends already compute these while drawing; exposed so the interaction layer can hit-test
   *  a click against them without recomputing density from scratch. Empty before the first
   *  render() with a non-null `overlays` frame. */
  hitCandidates(): readonly HitCandidate[];
  /** Releases GPU/canvas resources. The Renderer must not be used again after this. */
  dispose(): void;
}

export function amplitudeToUnit(sampleValue: number, scale: AmplitudeScale): number {
  const magnitude = Math.abs(sampleValue);
  const sign = sampleValue < 0 ? -1 : 1;
  switch (scale.type) {
    case 'linear':
      return sampleValue;
    case 'db': {
      // -60dB floor: quiet enough to be visually near-zero without a divide-by-zero at silence.
      const kFloorDb = -60;
      if (magnitude <= 0) return 0;
      const db = 20 * Math.log10(magnitude);
      return sign * clamp01((db - kFloorDb) / -kFloorDb);
    }
    case 'root':
      return sign * Math.pow(magnitude, 1 / Math.max(scale.param, 1));
  }
}

function clamp01(v: number): number {
  return Math.min(Math.max(v, 0), 1);
}

/** Which bands the current channel layout needs, and how many rows to lay them out in. Shared by
 *  both backends so layout math for 'split'/'overlaid'/'monoSum'/'midSide' only lives once. */
export function channelBandCount(layout: ChannelLayout, trackChannelCount: number): number {
  switch (layout) {
    case 'split':
      return Math.max(1, trackChannelCount);
    case 'overlaid':
      return 1;
    case 'monoSum':
      return 1;
    case 'midSide':
      return 2;
  }
}

export interface ChannelBand {
  /** Which waveform-query band (row within `bins`) this draw band reads from. */
  channelIndex: number;
  topDevicePx: number;
  heightDevicePx: number;
}

/**
 * Pixel-exact band layout shared by both backends (M17 follow-up: "mid/side and overlaid layout
 * polish" — the Canvas2D and WebGL backends previously each divided `devicePixelHeight / bandCount`
 * independently, leaving fractional band boundaries that didn't land on the same device pixel in
 * both backends and left hairline gaps/overlaps between adjacent bands. Every boundary here is
 * `Math.round`ed to an integer device pixel, and the *last* band absorbs whatever remainder that
 * rounding leaves, so the bands always tile `devicePixelHeight` exactly with a crisp, shared seam.
 */
export function layoutChannelBands(layout: ChannelLayout, trackChannelCount: number, devicePixelHeight: number): ChannelBand[] {
  const bandCount = layout === 'overlaid' ? 1 : channelBandCount(layout, trackChannelCount);
  const channelsToDraw = layout === 'overlaid' ? Math.max(1, trackChannelCount) : bandCount;

  const boundaries: number[] = [0];
  for (let band = 1; band <= bandCount; band++) {
    boundaries.push(band === bandCount ? devicePixelHeight : Math.round((devicePixelHeight * band) / bandCount));
  }

  const bands: ChannelBand[] = [];
  for (let i = 0; i < channelsToDraw; i++) {
    const bandIndex = layout === 'overlaid' ? 0 : i;
    bands.push({
      channelIndex: i,
      topDevicePx: boundaries[bandIndex]!,
      heightDevicePx: boundaries[bandIndex + 1]! - boundaries[bandIndex]!,
    });
  }
  return bands;
}
