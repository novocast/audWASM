// M18's unified marker model ("Decision — one model for everything, with kind-specific rendering,
// rather than bespoke overlays per analyser"). Every analyser-produced finding — beats, silence,
// clipping, transients, DC regions, chapters, decoder events, errors — and every user-created
// bookmark/annotation is a `Marker`. Hit-testing, navigation, filtering, export and the inspector
// all operate on this one shape; a new analyser only needs a registry entry here plus a draw
// function (renderer/overlays/drawOverlays.ts), per the M20 acceptance criterion in the task doc.

export type OverlayKind =
  | 'beat'
  | 'downbeat'
  | 'onset'
  | 'transient'
  | 'defect'
  | 'silence'
  | 'clipping'
  | 'dcRegion'
  | 'selection'
  | 'bookmark'
  | 'chapter'
  | 'cuePoint'
  | 'lyric'
  | 'decoderEvent'
  | 'error'
  | 'loopRegion';

export type Severity = 'info' | 'warning' | 'error';

export interface Marker {
  id: string;
  kind: OverlayKind;
  startFrame: number;
  /** Absent = point marker. */
  endFrame?: number;
  /** Channel-specific markers draw in that channel's lane; absent = applies to all channels. */
  channel?: number;
  severity?: Severity;
  label?: string;
  /** Kind-specific payload for the inspector (e.g. clip run length/peak, transient features). */
  data?: unknown;
  /** Survives re-analysis and cache round-trips (M18 "User-created markers"). */
  userCreated?: boolean;
}

/** How a kind behaves once its markers get dense at the current zoom (task doc's density table). */
export type DensityStrategy = 'aggregate' | 'thin' | 'region' | 'curve' | 'always';

/** Which lane a kind draws its own row in by default. Kinds sharing a lane id are drawn stacked
 *  in that lane; lane display metadata (order/height/label) lives in lanes.ts, keyed by this id. */
export type LaneId =
  | 'chapters'
  | 'beats'
  | 'waveform' // drawn over the waveform itself, not a separate lane (silence/clipping/dc/selection)
  | 'loudness'
  | 'transients'
  | 'lyrics'
  | 'errors';

export interface OverlayKindMeta {
  kind: OverlayKind;
  density: DensityStrategy;
  lane: LaneId;
  /** Hit-test tie-break priority — higher wins. Matches the doc's fixed order: errors > defects >
   *  user markers > analysis markers. */
  priority: number;
  /** Theme token name (see renderer/theme.ts) this kind's colour comes from when it has a
   *  dedicated one; kinds without a dedicated token (most analysis kinds) fall back to a
   *  severity-driven or lane-driven default in drawOverlays.ts. */
  themeToken?: string;
  /** Lanes/kinds hidden by default keep the timeline usable out of the box (mitigation for
   *  "visual noise" in the risk table) — the findings list still surfaces everything regardless. */
  defaultVisible: boolean;
}

// Priority bands, per the doc's fixed overlap-resolution order (highest first):
//   errors > defects > user markers > analysis markers
const kPriorityError = 40;
const kPriorityDefect = 30;
const kPriorityUser = 20;
const kPriorityAnalysis = 10;

export const kOverlayRegistry: Record<OverlayKind, OverlayKindMeta> = {
  error: { kind: 'error', density: 'always', lane: 'errors', priority: kPriorityError, defaultVisible: true },
  decoderEvent: { kind: 'decoderEvent', density: 'always', lane: 'errors', priority: kPriorityError - 1, defaultVisible: true },
  defect: { kind: 'defect', density: 'aggregate', lane: 'transients', priority: kPriorityDefect, defaultVisible: true },
  bookmark: { kind: 'bookmark', density: 'always', lane: 'chapters', priority: kPriorityUser, defaultVisible: true },
  loopRegion: { kind: 'loopRegion', density: 'region', lane: 'waveform', priority: kPriorityUser - 1, defaultVisible: true },
  selection: { kind: 'selection', density: 'region', lane: 'waveform', priority: kPriorityUser - 2, defaultVisible: true },
  chapter: { kind: 'chapter', density: 'region', lane: 'chapters', priority: kPriorityAnalysis + 5, defaultVisible: true },
  cuePoint: { kind: 'cuePoint', density: 'always', lane: 'chapters', priority: kPriorityAnalysis + 5, defaultVisible: true },
  lyric: { kind: 'lyric', density: 'thin', lane: 'lyrics', priority: kPriorityAnalysis + 4, defaultVisible: true },
  downbeat: { kind: 'downbeat', density: 'thin', lane: 'beats', priority: kPriorityAnalysis + 3, defaultVisible: true },
  beat: { kind: 'beat', density: 'thin', lane: 'beats', priority: kPriorityAnalysis + 2, defaultVisible: true },
  onset: { kind: 'onset', density: 'aggregate', lane: 'transients', priority: kPriorityAnalysis + 2, defaultVisible: false },
  transient: { kind: 'transient', density: 'aggregate', lane: 'transients', priority: kPriorityAnalysis + 1, defaultVisible: true },
  clipping: { kind: 'clipping', density: 'aggregate', lane: 'waveform', priority: kPriorityAnalysis, themeToken: 'clipping', defaultVisible: true },
  silence: { kind: 'silence', density: 'region', lane: 'waveform', priority: kPriorityAnalysis - 1, defaultVisible: true },
  dcRegion: { kind: 'dcRegion', density: 'region', lane: 'waveform', priority: kPriorityAnalysis - 1, defaultVisible: false },
};

export function metaFor(kind: OverlayKind): OverlayKindMeta {
  return kOverlayRegistry[kind];
}

/** Kinds whose markers are always point-in-time curve *samples* rather than discrete events —
 *  loudness/RMS/correlation/ODF/DC-level lanes are drawn from a separate `CurveSeries`, not from
 *  `Marker[]` (a curve is one series per lane, not thousands of individual markers). Listed here
 *  so store/density code can tell curve lanes apart from marker-array lanes without a second enum. */
export type CurveKind = 'loudness' | 'rms' | 'correlation' | 'odf' | 'dcLevel';

export interface CurveSeries {
  kind: CurveKind;
  /** Sample spacing, in source frames — matches the analyser's native hop size (e.g. M08's 100ms
   *  loudness window). */
  framesPerSample: number;
  startFrame: number;
  values: Float32Array;
  /** Display range for the lane's y-axis, e.g. [-60, 0] dBFS for loudness. */
  minValue: number;
  maxValue: number;
}
