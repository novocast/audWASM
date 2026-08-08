// M18's overlays layer draw implementation. Shared verbatim by both renderer backends (Canvas2D
// draws it on its own stacked `overlays` canvas; WebGL draws it on the same 2D `overlayCtx` it
// already shares with the ruler/selection layers) — there is exactly one draw path, matching M17's
// "Canvas2D is not a second-class citizen" stance and this doc's own acceptance criterion that
// overlays render correctly on both backends.
//
// Layout decision (v1 scope): the task doc's diagram reserves dedicated vertical space around the
// waveform for lane rows and draws "properties of the audio at a point in time" (clipping,
// silence, DC, loop/selection regions) *inside* the waveform layer. Actually carving that space out
// of the waveform's own height budget means teaching layoutChannelBands (renderer.ts) about lane
// heights, which is real M17-layout-integration work, not overlay-drawing work. For v1 this module
// instead anchors the "chapters"+"beats" lanes to the top of the canvas and the
// "loudness"+"transients"+"lyrics"+"errors" lanes to the bottom, and draws the waveform-anchored
// kinds (clipping/silence/dcRegion/loopRegion) as full-height strips over the whole canvas rather
// than inside the waveform bands specifically. Every lane/kind still has its own strip, is still
// density-managed, hit-testable and inspectable — only the "shares the waveform's exact pixel rows"
// polish is deferred; reserving real space from the waveform layer is a follow-up once this lands.

import { frameToPixel, type FrameToPixelParams } from '../coords.ts';
import { aggregateToPixels, regionsToPixelSpans, resampleCurveToPixels, thinToPixels } from '../../overlays/density.ts';
import { metaFor, type Marker, type OverlayKind } from '../../overlays/model.ts';
import { layoutLanes, type LaneRect } from '../../overlays/lanes.ts';
import type { HitCandidate } from '../../overlays/hitTest.ts';
import type { OverlayFrameData, RenderFrame } from '../renderer.ts';

const kWaveformAnchoredKinds: readonly OverlayKind[] = ['clipping', 'silence', 'dcRegion', 'loopRegion'];

function cssRgba(c: { r: number; g: number; b: number; a: number }, alphaOverride?: number): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${alphaOverride ?? c.a})`;
}

/** Fixed v1 palette for kinds without a dedicated theme token (model.ts's `themeToken`) — mirrors
 *  the theme's severity-adjacent colours (error red, warning amber) so a new install with no
 *  custom theme still reads sensibly. */
const kKindColor: Record<OverlayKind, { r: number; g: number; b: number; a: number }> = {
  error: { r: 1, g: 0.3, b: 0.3, a: 1 },
  decoderEvent: { r: 1, g: 0.6, b: 0.2, a: 1 },
  defect: { r: 1, g: 0.5, b: 0.2, a: 0.9 },
  bookmark: { r: 0.95, g: 0.85, b: 0.3, a: 1 },
  loopRegion: { r: 0.4, g: 0.9, b: 0.6, a: 0.18 },
  selection: { r: 0.37, g: 0.6, b: 1, a: 0.22 },
  chapter: { r: 0.6, g: 0.75, b: 1, a: 0.9 },
  cuePoint: { r: 0.6, g: 0.75, b: 1, a: 1 },
  lyric: { r: 0.8, g: 0.8, b: 0.85, a: 0.9 },
  downbeat: { r: 1, g: 1, b: 1, a: 0.9 },
  beat: { r: 1, g: 1, b: 1, a: 0.5 },
  onset: { r: 0.5, g: 0.9, b: 1, a: 0.7 },
  transient: { r: 0.5, g: 0.9, b: 1, a: 0.9 },
  clipping: { r: 1, g: 0.25, b: 0.25, a: 1 },
  silence: { r: 0.5, g: 0.5, b: 0.55, a: 0.18 },
  dcRegion: { r: 0.8, g: 0.6, b: 1, a: 0.18 },
};

export interface OverlayDrawResult {
  /** Hit candidates built this frame, for a caller (interaction.ts's wiring) that wants to hit-test
   *  the immediately-preceding draw without recomputing density from scratch. */
  hitCandidates: HitCandidate[];
}

/**
 * Draws every visible lane and waveform-anchored kind for the current viewport. `ctx` must already
 * be sized to `devicePixelWidth`x`devicePixelHeight`; this function does not clear it — callers
 * clear once per frame (both backends already do, to compose with what else shares the canvas).
 */
export function drawOverlaysLayer(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
  dpr: number,
): OverlayDrawResult {
  const overlays = frame.overlays;
  if (!overlays) return { hitCandidates: [] };

  const view: FrameToPixelParams = frame.view;
  const widthCss = devicePixelWidth / dpr;
  const hitCandidates: HitCandidate[] = [];

  drawWaveformAnchoredKinds(ctx, overlays, view, devicePixelWidth, devicePixelHeight, dpr, hitCandidates);

  const topLanes = layoutLanes(overlays.lanes.filter((l) => l.id === 'chapters' || l.id === 'beats'));
  const bottomLaneConfigs = overlays.lanes.filter((l) => l.id !== 'chapters' && l.id !== 'beats');
  const bottomLanes = layoutLanes(bottomLaneConfigs);
  const bottomTotalHeightCss = bottomLanes.length > 0 ? bottomLanes.at(-1)!.topCss + bottomLanes.at(-1)!.heightCss : 0;

  for (const rect of topLanes) {
    drawLane(ctx, overlays, rect, view, widthCss, rect.topCss * dpr, dpr, hitCandidates);
  }
  for (const rect of bottomLanes) {
    const topCss = devicePixelHeight / dpr - bottomTotalHeightCss + rect.topCss;
    drawLane(ctx, overlays, { ...rect, topCss }, view, widthCss, topCss * dpr, dpr, hitCandidates);
  }

  return { hitCandidates };
}

function drawWaveformAnchoredKinds(
  ctx: CanvasRenderingContext2D,
  overlays: OverlayFrameData,
  view: FrameToPixelParams,
  devicePixelWidth: number,
  devicePixelHeight: number,
  dpr: number,
  hitCandidates: HitCandidate[],
): void {
  const widthPx = Math.ceil(devicePixelWidth / dpr);
  for (const kind of kWaveformAnchoredKinds) {
    const startFrame = view.startFrame;
    const endFrame = view.startFrame + view.framesPerPixel * widthPx;
    const visible = overlays.markers.get(kind).filter((m) => overlapsRange(m, startFrame, endFrame));
    if (visible.length === 0) continue;

    const meta = metaFor(kind);
    const color = kKindColor[kind];
    if (meta.density === 'region') {
      const spans = regionsToPixelSpans(visible, view, widthPx);
      ctx.fillStyle = cssRgba(color);
      for (const span of spans) {
        const x = span.startPx * dpr;
        const w = Math.max(1, (span.endPx - span.startPx) * dpr);
        ctx.fillRect(x, 0, w, devicePixelHeight);
        hitCandidates.push({ marker: span.markers[0]!, pixelStart: span.startPx, pixelEnd: span.endPx, laneOrder: -1 });
      }
    } else if (meta.density === 'aggregate') {
      const bins = aggregateToPixels(visible, view, widthPx);
      for (const bin of bins) {
        const intensity = Math.min(1, bin.count / 8); // count, not mean — one event is still a full-alpha 1px tick
        ctx.fillStyle = cssRgba(color, Math.max(color.a * 0.5, intensity));
        ctx.fillRect(bin.pixel * dpr, 0, Math.max(1, dpr), devicePixelHeight);
        hitCandidates.push({ marker: bin.representative, pixelStart: bin.pixel, pixelEnd: bin.pixel + 1, laneOrder: -1 });
      }
    }
  }
}

function overlapsRange(m: Marker, startFrame: number, endFrame: number): boolean {
  const end = m.endFrame ?? m.startFrame;
  return end >= startFrame && m.startFrame <= endFrame;
}

function kindsForLane(laneId: LaneRect['lane']['id']): OverlayKind[] {
  switch (laneId) {
    case 'chapters':
      return ['chapter', 'cuePoint', 'bookmark'];
    case 'beats':
      return ['beat', 'downbeat'];
    case 'transients':
      return ['transient', 'onset', 'defect'];
    case 'lyrics':
      return ['lyric'];
    case 'errors':
      return ['error', 'decoderEvent'];
    case 'loudness':
    case 'waveform':
      return [];
  }
}

function drawLane(
  ctx: CanvasRenderingContext2D,
  overlays: OverlayFrameData,
  rect: LaneRect,
  view: FrameToPixelParams,
  widthCss: number,
  topDevicePx: number,
  dpr: number,
  hitCandidates: HitCandidate[],
): void {
  const heightDevicePx = rect.heightCss * dpr;
  if (rect.lane.collapsed) return; // collapsed lanes are just a strip; nothing to draw inside it

  if (rect.lane.id === 'loudness') {
    drawCurveLane(ctx, overlays, view, widthCss, topDevicePx, heightDevicePx, dpr);
    return;
  }

  const startFrame = view.startFrame;
  const endFrame = view.startFrame + view.framesPerPixel * widthCss;
  const laneOrder = rect.topCss;

  for (const kind of kindsForLane(rect.lane.id)) {
    const meta = metaFor(kind);
    const visible = overlays.markers.get(kind).filter((m) => overlapsRange(m, startFrame, endFrame));
    if (visible.length === 0) continue;
    const color = kKindColor[kind];

    const toDraw = meta.density === 'thin' ? thinToPixels(visible, view, 3) : visible;
    for (const m of toDraw) {
      const x = frameToPixel(m.startFrame, view) * dpr;
      if (x < -heightDevicePx || x > widthCss * dpr + heightDevicePx) continue;
      const isSelected = overlays.selectedMarkerId === m.id;
      ctx.fillStyle = cssRgba(color);
      const halfW = (isSelected ? 4 : 2.5) * dpr;
      ctx.beginPath();
      ctx.moveTo(x, topDevicePx);
      ctx.lineTo(x + halfW, topDevicePx + heightDevicePx);
      ctx.lineTo(x - halfW, topDevicePx + heightDevicePx);
      ctx.closePath();
      ctx.fill();
      if (isSelected) {
        ctx.strokeStyle = cssRgba({ r: 1, g: 1, b: 1, a: 0.9 });
        ctx.lineWidth = Math.max(1, dpr);
        ctx.stroke();
      }
      hitCandidates.push({ marker: m, pixelStart: x / dpr - 3, pixelEnd: x / dpr + 3, laneOrder });
    }
  }
}

function drawCurveLane(
  ctx: CanvasRenderingContext2D,
  overlays: OverlayFrameData,
  view: FrameToPixelParams,
  widthCss: number,
  topDevicePx: number,
  heightDevicePx: number,
  dpr: number,
): void {
  const loudness = overlays.curves.loudness;
  if (!loudness) return;
  const samples = resampleCurveToPixels(loudness, view, Math.ceil(widthCss));
  if (samples.length === 0) return;

  const range = Math.max(1e-6, loudness.maxValue - loudness.minValue);
  const yFor = (v: number): number => topDevicePx + heightDevicePx * (1 - (v - loudness.minValue) / range);

  ctx.fillStyle = 'rgba(120, 200, 255, 0.35)';
  ctx.beginPath();
  ctx.moveTo(samples[0]!.pixel * dpr, yFor(samples[0]!.max));
  for (const s of samples) ctx.lineTo(s.pixel * dpr, yFor(s.max));
  for (let i = samples.length - 1; i >= 0; i--) ctx.lineTo(samples[i]!.pixel * dpr, yFor(samples[i]!.min));
  ctx.closePath();
  ctx.fill();
}
