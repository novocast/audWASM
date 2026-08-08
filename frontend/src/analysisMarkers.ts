// Pure mappers from each analyser's engine.ts result shape to M18's unified `Marker`/`CurveSeries`
// model (frontend/src/overlays/model.ts). Kept separate from main.ts, and free of any WASM/engine
// import, so this is trivially unit-testable against plain literal fixtures — the same discipline
// as overlays/density.ts and friends.

import type { BeatsResult, ClippingResult, DcResult, SilenceModeResult, TransientsResult } from '../../bindings/wasm/engine.ts';
import type { Beat, ClipKind, DcChannelResult, DcPattern, Transient } from '../../bindings/wasm/aud_wasm.d.ts';
import type { CurveSeries, Marker, Severity } from './overlays/model.ts';

// ClipKind/DcPattern are `const enum`s in aud_wasm.d.ts (a declaration-only file with no runtime
// module behind it) — imported as types only, and mirrored numerically here as plain literals,
// matching the rest of the frontend's convention for these WASM-mirrored enums (see main.ts's
// channelsModeFor for the same pattern) rather than importing enum values Vite/esbuild can't
// resolve across a .d.ts boundary.
const kClipKindDigital = 0;
const kClipKindOverFullScale = 1;
const kClipKindNearClip = 2;
const kClipKindInterSamplePeak = 3;

const kDcPatternNone = 0;
const kDcPatternConstant = 1;
const kDcPatternDrifting = 2;
const kDcPatternSectional = 3;

/** BS.1770 momentary-loudness windows update every 100ms — see model.ts's CurveSeries doc comment
 *  ("matches the analyser's native hop size, e.g. M08's 100ms loudness window"). Not reported by
 *  LoudnessResult itself, so fixed here rather than threaded through every call site. */
const kMomentaryHopSeconds = 0.1;

export function beatsToMarkers(result: BeatsResult, idPrefix = 'beat'): Marker[] {
  return result.beats.map((b: Beat, i: number): Marker => {
    const isDownbeat = b.beatIndexInBar === 0;
    return {
      id: `${idPrefix}:${b.frame}:${i}`,
      kind: isDownbeat ? 'downbeat' : 'beat',
      startFrame: b.frame,
      ...(isDownbeat ? { label: 'downbeat' } : {}),
      data: { confidence: b.confidence, beatIndexInBar: b.beatIndexInBar },
    };
  });
}

function clipSeverity(kind: ClipKind): Severity {
  return kind === kClipKindOverFullScale || kind === kClipKindInterSamplePeak ? 'error' : 'warning';
}

function clipKindLabel(kind: ClipKind): string {
  switch (kind) {
    case kClipKindDigital:
      return 'digital clip';
    case kClipKindOverFullScale:
      return 'over full scale';
    case kClipKindNearClip:
      return 'near clip';
    case kClipKindInterSamplePeak:
      return 'inter-sample peak';
    default:
      return 'clip';
  }
}

export function clippingToMarkers(result: ClippingResult, idPrefix = 'clipping'): Marker[] {
  return result.events.map((e, i): Marker => ({
    id: `${idPrefix}:${e.beginFrame}:${i}`,
    kind: 'clipping',
    startFrame: e.beginFrame,
    ...(e.endFrame > e.beginFrame ? { endFrame: e.endFrame } : {}),
    channel: e.channel,
    severity: clipSeverity(e.kind),
    label: `${clipKindLabel(e.kind)} ${e.peakDbfs.toFixed(1)}dBFS`,
    data: e,
  }));
}

function dcSeverity(channel: DcChannelResult): Severity {
  return channel.headroomLostDb >= 1 ? 'warning' : 'info';
}

function dcPatternLabel(pattern: DcPattern): string {
  switch (pattern) {
    case kDcPatternNone:
      return 'none';
    case kDcPatternConstant:
      return 'constant';
    case kDcPatternDrifting:
      return 'drifting';
    case kDcPatternSectional:
      return 'sectional';
    default:
      return 'dc';
  }
}

/** One region per channel whose pattern is significant (not `None`), spanning the whole track —
 *  a v1 simplification; `stepLocations`-precise sub-regions for the Sectional case are a follow-up
 *  (see the plan's "deliberately deferred" list). */
export function dcToMarkers(result: DcResult, totalFrames: number, idPrefix = 'dc'): Marker[] {
  return result.channels
    .filter((c) => c.pattern !== kDcPatternNone)
    .map((c, i): Marker => ({
      id: `${idPrefix}:${c.pattern}:${i}`,
      kind: 'dcRegion',
      startFrame: 0,
      endFrame: totalFrames,
      channel: i,
      severity: dcSeverity(c),
      label: `${dcPatternLabel(c.pattern)} DC offset: ${c.offsetDbfs.toFixed(1)}dBFS`,
      data: c,
    }));
}

function transientSeverity(t: Transient): Severity | undefined {
  if (t.classification === 'dropout') return 'error';
  if (t.classification === 'click') return 'warning';
  return undefined; // musical transients aren't problems
}

export interface TransientMarkers {
  transients: Marker[];
  defects: Marker[];
}

export function transientsToMarkers(result: TransientsResult, idPrefix = 'transient'): TransientMarkers {
  const toMarker = (t: Transient, i: number, kind: 'transient' | 'defect'): Marker => {
    const severity = transientSeverity(t);
    return {
      id: `${idPrefix}:${kind}:${t.attackFrame}:${i}`,
      kind,
      startFrame: t.attackFrame,
      ...(severity ? { severity } : {}),
      label: `${t.classification} ${(t.classConfidence * 100).toFixed(0)}%`,
      data: t,
    };
  };
  return {
    transients: result.transients.map((t, i) => toMarker(t, i, 'transient')),
    defects: result.defects.map((t, i) => toMarker(t, i, 'defect')),
  };
}

export function silenceToMarkers(mode: SilenceModeResult, idPrefix = 'silence'): Marker[] {
  return mode.regions.map((r, i): Marker => ({
    id: `${idPrefix}:${r.beginFrame}:${i}`,
    kind: 'silence',
    startFrame: r.beginFrame,
    endFrame: r.endFrame,
    severity: 'info',
    label: `${r.kind} silence (${r.position})`,
    data: r,
  }));
}

/** `momentaryLufs` is LoudnessResult.momentaryLufs. Non-finite windows (below the absolute gate,
 *  which some builds report as -Infinity) are excluded from the display-range calculation so one
 *  gated window doesn't blow out the whole lane's scale. */
export function loudnessToCurve(momentaryLufs: Float32Array, sampleRate: number): CurveSeries {
  const finite = [...momentaryLufs].filter((v) => Number.isFinite(v));
  const minValue = finite.length ? Math.min(...finite) - 3 : -60;
  const maxValue = finite.length ? Math.max(...finite) + 3 : 0;
  return {
    kind: 'loudness',
    framesPerSample: Math.round(kMomentaryHopSeconds * sampleRate),
    startFrame: 0,
    values: momentaryLufs,
    minValue,
    maxValue,
  };
}
