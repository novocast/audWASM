// Hand-written .d.ts — deliberately not Embind's auto-generated types, which are too loose
// (everything comes back as `any`/`number`). See M01.

export interface EngineBuildInfo {
  version: string;
  simd: boolean;
  threads: boolean;
  optimisation: string;
}

export interface SelfTestResult {
  pass: boolean;
  code?: string;
  detail?: string;
}

export interface StreamInfo {
  ok: boolean;
  sampleRate: number;
  channels: number;
  /** -1 means unknown until fully decoded. */
  frameCount: number;
  codecName: string;
  bitDepth: number;
  nominalBitrate: number;
  isLossy: boolean;
  isEstimate: boolean;
  encoderDelayFrames: number;
  encoderPaddingFrames: number;
}

export type DiagnosticSeverity = 0 | 1 | 2; // Info | Warning | Error, see decoder.hpp

export interface DecodeDiagnostic {
  severity: DiagnosticSeverity;
  code: string;
  byteOffset: number;
  frameIndex: number;
  message: string;
}

export interface OperationResult {
  ok: boolean;
  code?: string;
  detail?: string;
}

/** Raw Embind handle. Prefer the DecodeSession wrapper in engine.ts — this is what it wraps. */
export interface RawDecodeSessionHandle {
  feedBytes(ptr: number, length: number): OperationResult;
  finish(): OperationResult;
  getStreamInfo(): StreamInfo;
  getDecodedFrameCount(): number;
  getDiagnostics(): DecodeDiagnostic[];
  /** Opaque non-owning pointer to the session's aud::AudioBuffer; pass to RawTransport.attachSource(). */
  audioBufferHandle(): number;
  /** Copies decoded PCM for `channel` in [beginFrame, endFrame) into the heap at `outPtr` (which
   *  must already hold room for (endFrame-beginFrame) float32s). Used to hand the spectrogram
   *  worker its own copy of the track (M07). */
  readPlanarChannel(channel: number, beginFrame: number, endFrame: number, outPtr: number): OperationResult;
  delete(): void; // Embind-generated; required to release the underlying C++ object
}

// See engine/playback/transport.hpp's TransportStatus for the source of truth.
export type TransportStatusName =
  | 'idle'
  | 'loading'
  | 'ready'
  | 'playing'
  | 'paused'
  | 'seeking'
  | 'ended';

export interface RawTransportState {
  status: TransportStatusName;
  positionFrames: number;
  /** -1 (kNoFrame) while unknown. */
  durationFrames: number;
  loopEnabled: boolean;
  loopBegin: number;
  loopEnd: number;
  gain: number;
  outputSampleRate: number;
}

/** Raw Embind handle. Prefer the TransportClient wrapper in playback/transportClient.ts. */
export interface RawTransportHandle {
  attachSource(audioBufferHandle: number): void;
  setSourceComplete(complete: boolean): void;
  dispatchLoad(): OperationResult;
  dispatchReady(durationFrames: number): OperationResult;
  dispatchPlay(): OperationResult;
  dispatchPause(): OperationResult;
  dispatchSeekTo(targetFrame: number): OperationResult;
  dispatchSetLoopRange(beginFrame: number, endFrame: number): OperationResult;
  dispatchSetLoopEnabled(enabled: boolean): OperationResult;
  dispatchSetLoopCrossfadeFrames(frames: number): OperationResult;
  dispatchSetGain(gain: number): OperationResult;
  dispatchReset(): OperationResult;
  /** Pulls up to `maxFrames` resampled frames from the source into the internal ring. */
  pump(maxFrames: number): number;
  /** Copies up to `framesRequested` frames (planar-contiguous per channel) to heap ptr `outPtr`. */
  renderInto(outPtr: number, framesRequested: number): number;
  getState(): RawTransportState;
  ringFramesAvailable(): number;
  delete(): void;
}

// See engine/waveform/waveform_store.hpp's ChannelSelector for the source of truth. A plain union
// (not a TS enum) to match this file's DiagnosticSeverity convention — isolatedModules (Vite/esbuild
// transpile each file independently) can't inline `const enum` member access, and a normal enum
// would emit a runtime object this ambient .d.ts has no corresponding .js for.
export type WaveformChannelSelector = 0 | 1 | 2; // PerChannel | MonoSum | MidSide

export interface WaveformOperationResult {
  ok: boolean;
  code?: string;
  detail?: string;
}

/** `ptr` points at a WaveformBin[binCount] in the WASM heap: 4 float32s per bin
 *  (min, max, rms, absPeak) — see bindings/wasm/waveformView.ts for the typed view over it. */
export interface WaveformBinsResult extends WaveformOperationResult {
  ptr?: number;
  binCount?: number;
}

export interface WaveformQueryResult extends WaveformOperationResult {
  ptr?: number;
  channels?: number;
  binCount?: number;
  framesPerBin?: number;
  isComplete?: boolean;
  /** True if the requested resolution was finer than the pyramid's level 0, so the engine reduced
   *  raw PCM directly rather than aggregating pyramid bins (M05 "Below level 0"). */
  isRawPcm?: boolean;
}

/** Raw Embind handle. Prefer the Waveform wrapper in engine.ts. */
export interface RawWaveformHandle {
  processAvailableChunks(): WaveformOperationResult;
  finish(): WaveformOperationResult;
  channelCount(): number;
  isComplete(): boolean;
  levelZeroBins(channel: number): WaveformBinsResult;
  monoSumBins(): WaveformBinsResult;
  midBins(): WaveformBinsResult;
  sideBins(): WaveformBinsResult;
  query(
    channelsMode: WaveformChannelSelector,
    rangeBegin: number,
    rangeEnd: number,
    binCount: number,
  ): WaveformQueryResult;
  delete(): void;
}

// See engine/fft/windows.hpp's WindowType / engine/fft/scaling.hpp's SpectrumScaling for the
// source of truth — raw integer values of those enums, same convention used throughout M06/M07's
// bindings.
export type FftWindowType = 0 | 1 | 2 | 3 | 4 | 5; // Rectangular|Hann|Hamming|Blackman|BlackmanHarris|Kaiser
export type FftSpectrumScaling = 0 | 1 | 2 | 3; // Raw|Amplitude|Power|PowerDensity

export interface StftFramesResult extends OperationResult {
  ptr?: number;
  binCount?: number;
  frameCount?: number;
}

/** Raw Embind handle. M06's STFT engine — batches every frame completed by process()/finish() into
 *  one contiguous heap buffer (frame 0's bins, then frame 1's, ...), per M06's "never materialise
 *  the whole STFT" rule; callers drive it incrementally. */
export interface RawStftHandle {
  process(ptr: number, length: number): StftFramesResult;
  finish(): StftFramesResult;
  binCount(): number;
  frameTimeSeconds(frameIndex: number): number;
  binFrequencyHz(bin: number): number;
  frameCountFor(totalFrames: number): number;
  delete(): void;
}

/** Raw Embind handle. Builds a standalone aud::AudioBuffer from planar PCM copied into the heap,
 *  independent of DecodeSession — used by the spectrogram worker (M07) to hold its own copy of the
 *  decoded PCM. See engine bindings/spectrogram_bindings.cpp's PcmBufferHandle. */
export interface RawPcmBufferHandle {
  /** `channelPtrsPtr` points at `channelCount` heap pointers, each pointing at `frames` float32
   *  samples already copied into the heap (one bulk transfer per channel, handed over in one call). */
  appendPlanar(channelPtrsPtr: number, channelCount: number, frames: number): OperationResult;
  audioBufferHandle(): number;
  delete(): void;
}

// See engine/spectrogram/tile.hpp's FreqAxis / Decimation for the source of truth.
export type SpectrogramFreqAxis = 0 | 1 | 2 | 3; // Linear|Log|Mel|Bark
export type SpectrogramDecimation = 0 | 1; // Max|Mean

export interface SpectrogramSetConfigResult extends OperationResult {
  configHash?: number;
}

/** `ptr` points at kTileWidth*kTileHeight (256*256) quantised dB bytes; row 0 = lowest displayed
 *  frequency. See engine/spectrogram/tile.hpp. */
export interface SpectrogramTileResult extends OperationResult {
  ptr?: number;
  byteLength?: number;
  floorDb?: number;
  ceilDb?: number;
}

export interface SpectrogramOverviewResult extends OperationResult {
  ptr?: number;
  width?: number;
  height?: number;
  floorDb?: number;
  ceilDb?: number;
}

export interface SpectrogramPointResult extends OperationResult {
  frequencyHz?: number;
  magnitudeDb?: number;
}

/** Raw Embind handle. Prefer the Spectrogram wrapper in engine.ts. Meant to be constructed inside
 *  the dedicated spectrogram worker (M07), against a copy of the decoded PCM the worker owns —
 *  see frontend/workers/spectrogramWorker.ts. */
export interface RawSpectrogramHandle {
  setConfig(
    fftSize: number,
    window: FftWindowType,
    scaling: FftSpectrumScaling,
    freqAxis: SpectrogramFreqAxis,
    decimation: SpectrogramDecimation,
    minHz: number,
    floorDb: number,
    ceilDb: number,
  ): SpectrogramSetConfigResult;
  currentConfigHash(): number;
  requestTile(level: number, tileX: number, channel: number): SpectrogramTileResult;
  invalidateConfig(staleConfigHash: number): void;
  cacheTileCount(): number;
  cacheCurrentBytes(): number;
  cacheByteBudget(): number;
  overview(channel: number): SpectrogramOverviewResult;
  queryPoint(channel: number, timeSeconds: number, targetHz: number): SpectrogramPointResult;
  delete(): void;
}

// M08's LoudnessAnalyzer result, handed back as scalars plus {ptr,length} heap views over the
// per-channel peak arrays (float64) and the momentary/short-term time series (float32) — never
// per-sample calls, per M01's binding convention.
export interface LoudnessFinishResult extends OperationResult {
  integratedLufs?: number;    // NaN if nothing survived the two-stage gate (e.g. pure silence)
  loudnessRangeLu?: number;
  truePeakDbtp?: number;      // max over channels
  samplePeakDbfs?: number;
  truePeakFrame?: number;
  truePeakOversampling?: number;
  usedFallbackChannelLayout?: boolean;

  truePeakPerChannelPtr?: number;
  truePeakPerChannelCount?: number;
  samplePeakPerChannelPtr?: number;
  samplePeakPerChannelCount?: number;

  momentaryPtr?: number;   // float32[momentaryCount], 100 ms resolution
  momentaryCount?: number;
  shortTermPtr?: number;   // float32[shortTermCount], 100 ms resolution
  shortTermCount?: number;
}

/** Raw Embind handle. Prefer the Loudness wrapper in engine.ts. Driven directly against a
 *  DecodeSession's audioBufferHandle(), same polling contract as RawWaveformHandle. */
export interface RawLoudnessHandle {
  processAvailableChunks(): OperationResult;
  finish(): LoudnessFinishResult;
  gainToTargetDb(targetLufs: number): number;
  delete(): void;
}

/** One channel's worth of M09 statistics — see docs/report-schema.json's `channelStatistics`. */
export interface StatisticsChannelResult {
  peak: number;
  peakDbfs: number;
  peakFrame: number;
  minValue: number;
  maxValue: number;
  dcOffset: number;
  rms: number;
  rmsDbfs: number;
  variance: number;
  stdDev: number;
  crestFactorDb: number;
  zeroCrossingRate: number;
  effectiveBitDepth: number | null;
  containerBitDepth: number;
  ditherLikely: boolean;
  ditherConfidence: number;
  bitDepthDescription: string;
  histogramPtr: number;   // uint32[1024]
  histogramCount: number;
}

export interface StatisticsStereoResult {
  correlation: number;
  balanceDb: number;
  monoCompatibilityDb: number;
  correlationSeriesPtr: number;   // float32[correlationSeriesCount], 50 ms resolution
  correlationSeriesCount: number;
}

export interface StatisticsFinishResult extends OperationResult {
  sampleRate?: number;
  channelCount?: number;
  frameCount?: number;
  crestFactorDb?: number;
  dynamicRangeDr?: number;
  /** The full report, serialised exactly per docs/report-schema.json — the stable public artifact. */
  reportJson?: string;
  channels?: StatisticsChannelResult[];
  stereo?: StatisticsStereoResult | null;
  rmsSeriesPtr?: number;   // float32[rmsSeriesCount], 50 ms resolution, interleaved by channel
  rmsSeriesCount?: number;
  rmsSeriesChannelCount?: number;
  /** Same interleaving/grid as rmsSeries: 1 if every sample in that window was exactly zero — feeds
   *  M10's digital-silence mode. */
  allZeroSeriesPtr?: number;   // uint8[allZeroSeriesCount]
  allZeroSeriesCount?: number;
}

/** Raw Embind handle. Prefer the Statistics wrapper in engine.ts. Driven directly against a
 *  DecodeSession's audioBufferHandle(), same polling contract as RawWaveformHandle/RawLoudnessHandle. */
export interface RawStatisticsHandle {
  processAvailableChunks(): OperationResult;
  finish(): StatisticsFinishResult;
  delete(): void;
}

/** M10's SilencePosition enum, mirrored numerically (aud::silence::SilencePosition). */
export const enum SilencePosition {
  Leading = 0,
  Internal = 1,
  Trailing = 2,
  EntireFile = 3,
}

/** M10's SilenceKind enum, mirrored numerically (aud::silence::SilenceKind). */
export const enum SilenceKind {
  Digital = 0,
  Threshold = 1,
  Perceptual = 2,
}

export interface SilenceRegionResult {
  beginFrame: number;
  endFrame: number;
  startSeconds: number;
  endSeconds: number;
  kind: SilenceKind;
  position: SilencePosition;
  /** Coarse (window-average) until refineThreshold()/refineDigital() has run for this region's
   *  kind, sample-accurate afterwards — see M10 "Boundary refinement". */
  peakDbfsWithin: number;
  rmsDbfsWithin: number;
  /** Bit c set => channel c was silent at some point in this region (meaningful for channelMode
   *  "Any"). */
  channelMask: number;
}

export interface SilenceParametersUsed {
  thresholdDb: number;
  minDurationMs: number;
  mergeGapMs: number;
  channelModeAny: boolean;
  useHysteresis: boolean;
  hysteresisDb: number;
}

export interface SilenceModeResult extends OperationResult {
  regions?: SilenceRegionResult[];
  leadingSilenceSeconds?: number;
  trailingSilenceSeconds?: number;
  totalSilenceSeconds?: number;
  silenceFraction?: number;
  parametersUsed?: SilenceParametersUsed;
}

export interface SilenceDetectResult extends OperationResult {
  threshold?: SilenceModeResult;
  digital?: SilenceModeResult;
  perceptual?: SilenceModeResult;
}

/** Raw Embind handle. Prefer the Silence wrapper in engine.ts. Unlike Waveform/Loudness/Statistics,
 *  detect() is a pure, instant recompute over series copied in at create() time — no PCM access,
 *  safe on every slider tick (M10 "reparameterisation ... instantly, on the main thread").
 *  refineThreshold()/refineDigital() are the separate PCM-touching step; call once, debounced,
 *  after the user stops dragging, and only after at least one detect() call. */
export interface RawSilenceHandle {
  detect(
    thresholdDb: number,
    minDurationMs: number,
    mergeGapMs: number,
    channelModeAny: boolean,
    useHysteresis: boolean,
    hysteresisDb: number,
    perceptualGateLufs: number,
  ): SilenceDetectResult;
  refineThreshold(): SilenceModeResult;
  refineDigital(): SilenceModeResult;
  delete(): void;
}

/** M11's ClipKind enum, mirrored numerically (aud::clipping::ClipKind). */
export const enum ClipKind {
  Digital = 0,
  OverFullScale = 1,
  NearClip = 2,
  InterSamplePeak = 3,
}

export interface ClipEventResult {
  beginFrame: number;
  endFrame: number;
  startSeconds: number;
  endSeconds: number;
  channel: number;
  kind: ClipKind;
  /** Linear amplitude; may exceed 1.0 (OverFullScale) or represent a dBTP-domain peak (InterSamplePeak). */
  peakValue: number;
  /** dBFS for Digital/OverFullScale/NearClip; dBTP for InterSamplePeak. */
  peakDbfs: number;
  /** Length of the flat run in samples; always 1 for InterSamplePeak point events. */
  sampleCount: number;
}

export interface ClippingFinishResult extends OperationResult {
  totalClippedSamples?: number;
  clippedFraction?: number;
  maxOvershootDb?: number;
  flatTopRatio?: number;
  meanPlateauLength?: number;
  heavyLimitingLikely?: boolean;
  containerBitDepth?: number;
  /** The full report, serialised as {@link ClippingResult}'s toJson(). */
  reportJson?: string;
  /** Uncapped counts indexed by ClipKind — always show these, even once `events` is capped. */
  eventCount?: [number, number, number, number];
  /** Capped at the `maxStoredEvents` passed to configure() (default 10000), keeping the worst
   *  events by overshoot — see `eventsCapped`. */
  events?: ClipEventResult[];
  /** True if `events.length` is less than the sum of `eventCount` — the UI should say "showing
   *  worst N of TOTAL" rather than imply `events` is exhaustive. */
  eventsCapped?: boolean;
  /** Per-bin clipping intensity (0..1), aligned to the waveform pyramid's level-0 bin size
   *  (densityBinFrames, aud::waveform::kBaseBinFrames) — see bindings/wasm/spectrogramView.ts-style
   *  typed views for the convention. */
  densitySeriesPtr?: number;   // float32[densitySeriesCount]
  densitySeriesCount?: number;
  densityBinFrames?: number;
}

/** Raw Embind handle. Prefer a Clipping wrapper in engine.ts once one is added (none of
 *  Loudness/Statistics/Silence has one yet either — see engine.ts). Driven directly against a
 *  DecodeSession's audioBufferHandle(), same polling contract as RawStatisticsHandle. */
export interface RawClippingHandle {
  /** Must be called before the first processAvailableChunks() — re-derives thresholds from the
   *  new parameters and resets the chunk cursor. `nearClipDbfs`/`flatnessToleranceDb` are dB;
   *  everything else is a sample/frame count or a 4/8/16 oversampling factor (0 defaults to 4). */
  configure(
    minRunSamples: number,
    nearClipDbfs: number,
    nearClipMinRun: number,
    flatnessToleranceDb: number,
    mergeGapSamples: number,
    ispOversampling: number,
    detectInterSamplePeaks: boolean,
    maxStoredEvents: number,
  ): void;
  processAvailableChunks(): OperationResult;
  finish(): ClippingFinishResult;
  delete(): void;
}

/** M12's DcPattern enum, mirrored numerically (aud::dc::DcPattern). */
export const enum DcPattern {
  None = 0,
  Constant = 1,
  Drifting = 2,
  Sectional = 3,
}

export interface DcChannelResult {
  offsetLinear: number;
  offsetDbfs: number;
  offsetPercent: number;
  pattern: DcPattern;
  /** Min/max of the 1s windowed series — the regression guard for a global mean that hides a
   *  sectional or drifting file (M12's risk table). */
  minWindowOffset: number;
  maxWindowOffset: number;
  headroomLostDb: number;
  /** Analytic, no second decode pass: subtracting the constant offset shifts min/max by exactly
   *  that constant (M12 "Preview metrics"). */
  peakAfterCorrectionDbfs: number;
  /** 0 if a constant subtraction suffices; otherwise a recommended 2nd-order Butterworth cutoff
   *  (typically 5-20 Hz) for the Drifting case. */
  recommendedHighpassHz: number;
  /** Frame indices of detected step changes; populated only when pattern === Sectional. */
  stepLocations: number[];
}

export interface DcFinishResult extends OperationResult {
  significanceThresholdDbfs?: number;
  anySignificant?: boolean;
  windowSeconds?: number;
  /** The full report, serialised as {@link DcOffsetResult}'s toJson(). */
  reportJson?: string;
  channels?: DcChannelResult[];
  /** 1s-resolution windowed mean, interleaved by channel exactly like Statistics's rmsSeries. */
  windowSeriesPtr?: number;   // float32[windowSeriesCount]
  windowSeriesCount?: number;
  windowSeriesChannelCount?: number;
}

/** Raw Embind handle. Prefer a Dc wrapper in engine.ts once one is added (none of Clipping has one
 *  yet either — see engine.ts). Driven directly against a DecodeSession's audioBufferHandle(),
 *  same polling contract as RawStatisticsHandle/RawClippingHandle. Never writes corrected audio —
 *  everything here is measurement plus analytic preview (M12: "the engine computes correction
 *  *parameters* and previewed *metrics*; it never writes corrected audio in v1"). */
export interface RawDcHandle {
  /** Must be called before the first processAvailableChunks() to use a non-default significance
   *  threshold (default -60 dBFS) — re-begins the analyser, same convention as
   *  RawClippingHandle.configure(). */
  configure(significanceThresholdDbfs: number): void;
  processAvailableChunks(): OperationResult;
  finish(): DcFinishResult;
  delete(): void;
}

export interface BeatOnset {
  timeSeconds: number;
  frame: number;
  strength: number;
  /** Which frequency bands contributed: bit0=low bit1=mid bit2=high (aud::beats::Onset::bandMask). */
  bandMask: number;
}

export interface Beat {
  timeSeconds: number;
  frame: number;
  confidence: number;
  /** -1 == unknown (M13 v1 emits beats only — see the task doc's "Downbeat detection — deferred");
   *  0 == downbeat. Only set once a manual "set downbeat" edit has been applied. */
  beatIndexInBar: number;
}

export interface TempoCandidateResult {
  bpm: number;
  score: number;
}

export interface BeatFinishResult extends OperationResult {
  primaryBpm?: number;
  /** 0..1 — peakiness of the tempo autocorrelation. Low on ambiguous/noisy material; the UI must
   *  show this, not just the BPM (M13's "Be honest about what this is"). */
  tempoConfidence?: number;
  /** 0..1 — how well the beat grid lands on strong ODF peaks, independent of tempoConfidence:
   *  a track can have a confident tempo with the wrong downbeat phase. */
  phaseConfidence?: number;
  /** false if tempo varies meaningfully across ~10s windows (live performance / tempo ramp) — a
   *  varying track must not be presented with a single confident BPM. */
  tempoIsStable?: boolean;
  odfHopSeconds?: number;
  /** The full report, serialised as {@link BeatResult}'s toJson(). */
  reportJson?: string;
  onsets?: BeatOnset[];
  beats?: Beat[];
  /** Runners-up including the primary, sorted by score desc — always includes the x2/÷2/x1.5
   *  alternatives when their score is close (M13's octave-ambiguity handling). */
  alternatives?: TempoCandidateResult[];
  /** The combined, normalised ODF — retained so the UI can draw it under the waveform and
   *  re-threshold instantly (M13: "Retaining the ODF is deliberate"). */
  odfPtr?: number;   // float32[odfCount]
  odfCount?: number;
  /** Primary BPM per ~10s window — see tempoIsStable. */
  tempoSeriesPtr?: number;   // float32[tempoSeriesCount]
  tempoSeriesCount?: number;
}

/** Raw Embind handle. Driven directly against a DecodeSession's audioBufferHandle(), same polling
 *  contract as RawStatisticsHandle/RawDcHandle. Runs its own STFT pass (M06) internally. */
export interface RawBeatsHandle {
  /** Must be called before the first processAvailableChunks() to use non-default parameters
   *  (0 keeps that parameter's default). */
  configure(fftSize: number, hopSize: number, timeSignatureBeatsPerBar: number): void;
  processAvailableChunks(): OperationResult;
  finish(): BeatFinishResult;
  /** Manual-edit merge (M13's "Editability") — never mutates the detected result from finish().
   *  `addedPtr`/`removedPtr` are HEAPF64 arrays of timeSeconds (0/0 for "none"). Pass
   *  `tempoOverrideBpm <= 0` / `hasDownbeat = false` to skip those edits. */
  applyEdits(
    tempoOverrideBpm: number,
    phaseNudgeSeconds: number,
    downbeatTimeSeconds: number,
    hasDownbeat: boolean,
    addedPtr: number,
    addedCount: number,
    removedPtr: number,
    removedCount: number,
    timeSignatureBeatsPerBar: number,
  ): BeatFinishResult;
  delete(): void;
}

/** aud::transients::TransientClass, serialised as a string (see M14's classifier.hpp header
 *  comment — the rule-based classifier only ever produces Kick/Snare/HiHat/Percussion/TonalOnset/
 *  Unclassified; Click/Dropout come from the click/dropout detectors, not the classifier). */
export type TransientClassification =
  | 'kick'
  | 'snare'
  | 'hiHat'
  | 'percussion'
  | 'tonalOnset'
  | 'click'
  | 'dropout'
  | 'unclassified';

export interface Transient {
  startFrame: number;
  attackFrame: number;
  startSeconds: number;
  attackSeconds: number;
  classification: TransientClassification;
  /** 0..1. For classification === 'unclassified', this reflects how clearly nothing matched, not a
   *  guess at a class (M14: "reported honestly rather than forced into a bucket"). */
  classConfidence: number;
  /** Normalised; carried through from the M13 onset candidate for musical transients, 1.0 for
   *  defects (clicks/dropouts don't have an onset-detector strength to inherit). */
  strength: number;
  peakDbfs: number;
  attackTimeMs: number;
  /** For a Dropout, this is the run's duration rather than a decay measurement. */
  decayTimeMs: number;
  spectralCentroidHz: number;
  /** 0 (tonal) .. 1 (noisy/flat). */
  spectralFlatness: number;
  /** [low(<150Hz), lowMid(150Hz-1kHz), mid(1kHz-5kHz), high(>5kHz)], each a fraction of total band
   *  energy. */
  bandEnergyRatio: [number, number, number, number];
}

export interface TransientFinishResult extends OperationResult {
  /** The full report, serialised as {@link TransientResult}'s toJson(). */
  reportJson?: string;
  /** Musical transients only — defects are surfaced separately (M14: "a user checking a master for
   *  clicks does not want to scroll past 4 000 hi-hats"). */
  transients?: Transient[];
  /** Click + Dropout, in one list so the UI can show a QA-focused defect panel without filtering. */
  defects?: Transient[];
  /** Indexed by TransientClass (Kick=0, Snare=1, HiHat=2, Percussion=3, TonalOnset=4, Click=5,
   *  Dropout=6, Unclassified=7), counted across both transients and defects. */
  countByClass?: number[];
}

/** Raw Embind handle. Driven directly against a DecodeSession's audioBufferHandle(), same polling
 *  contract as RawStatisticsHandle/RawDcHandle/RawBeatsHandle. Consumes M13's onset list as
 *  candidates rather than running a second onset detector — see M14's transient_analyzer.hpp header
 *  comment. */
export interface RawTransientsHandle {
  processAvailableChunks(): OperationResult;
  finish(): TransientFinishResult;
  delete(): void;
}

// aud::metadata::ReplayGainSource — see M15's ReplayGainInfo comment: sources are kept independent
// (Vorbis/TXXX > RVA2 > iTunNORM priority order), never merged.
export interface ReplayGainSourceResult {
  origin: string;
  trackGainDb: number | null;
  trackPeak: number | null;
  albumGainDb: number | null;
  albumPeak: number | null;
}

export interface ReplayGainResult {
  sources: ReplayGainSourceResult[];
}

/** aud::metadata::PictureType, numerically (0=Other, 3=FrontCover, 4=BackCover, ... see
 *  metadata.hpp for the full enum). */
export type PictureTypeCode = number;

/** Metadata *about* one embedded picture — the pixel bytes are fetched separately via
 *  Metadata.getPictureBytes(index) (M15: never decoded in C++; handed to the browser as bytes). */
export interface PictureInfo {
  index: number;
  declaredMimeType: string;
  detectedMimeType: string;
  /** True when the tag's declared MIME type doesn't match what the magic bytes indicate. */
  mimeMismatch: boolean;
  type: PictureTypeCode;
  description: string;
  sourceFormat: string;
  byteCount: number;
}

export interface LyricLineResult {
  /** Negative for an unsynced line (no known position). */
  timeSeconds: number;
  text: string;
}

export interface LyricsResult {
  synced: boolean;
  language: string;
  description: string;
  sourceFormat: string;
  lines: LyricLineResult[];
}

export interface CuePointResult {
  timeSeconds: number;
  label: string;
  sourceFormat: string;
}

/** BWF `bext` chunk — see M15 "Broadcast metadata (BWF)". Loudness fields are null on BWF v0/v1
 *  (they were added in v2). */
export interface BroadcastInfoResult {
  present: boolean;
  description: string;
  originator: string;
  originatorReference: string;
  originationDate: string;
  originationTime: string;
  /** Sample-accurate timecode start, in samples at the file's own rate. */
  timeReference: number;
  version: number;
  /** 64-byte UMID, hex-encoded. */
  umid: string;
  codingHistory: string;
  loudnessValueLufs: number | null;
  loudnessRangeLu: number | null;
  maxTruePeakDbtp: number | null;
  maxMomentaryLufs: number | null;
  maxShortTermLufs: number | null;
}

/** One raw tag entry preserved verbatim — either something no common field maps onto
 *  (`unmapped`, `rawKey` present) or one side of a cross-format disagreement on a mapped field
 *  (`fieldConflicts`, keyed by field name, `rawKey` absent). M15: "always preserve unmapped tags" /
 *  "report conflicts rather than resolving them silently". */
export interface MetadataValueEntry {
  key: string;
  text: string;
  sourceFormat: string;
  rawKey?: string;
}

export type MetadataDiagnosticSeverity = 0 | 1 | 2; // Info | Warning | Error, see metadata.hpp's Severity

export interface MetadataDiagnostic {
  severity: MetadataDiagnosticSeverity;
  message: string;
}

export interface MetadataResult {
  ok: boolean;
  title: string | null;
  artist: string | null;
  albumArtist: string | null;
  album: string | null;
  genre: string | null;
  composer: string | null;
  comment: string | null;
  publisher: string | null;
  copyright: string | null;
  encodedBy: string | null;
  encoderSettings: string | null;
  year: number | null;
  trackNumber: number | null;
  trackTotal: number | null;
  discNumber: number | null;
  discTotal: number | null;
  bpm: number | null;
  isrc: string | null;
  upc: string | null;
  catalogNumber: string | null;
  musicBrainzTrackId: string | null;
  musicBrainzAlbumId: string | null;
  date: string | null;
  replayGain: ReplayGainResult;
  pictures: PictureInfo[];
  lyrics: LyricsResult[];
  cuePoints: CuePointResult[];
  broadcast: BroadcastInfoResult;
  unmapped: MetadataValueEntry[];
  fieldConflicts: MetadataValueEntry[];
  diagnostics: MetadataDiagnostic[];
}

/** Raw Embind handle. Prefer the Metadata wrapper in engine.ts — this is what it wraps. One-shot,
 *  not a streaming Analyzer: metadata is read from the raw encoded file bytes directly (see M15),
 *  closer in shape to DecodeSession than to Loudness/Dc/etc. */
export interface RawMetadataHandle {
  result(): MetadataResult;
  /** Zero-copy pointer into picture `index`'s still-owned bytes; 0 if `index` is out of range.
   *  Valid only until this handle is deleted (or another allocating engine call runs) — the TS
   *  wrapper's getPictureBytes() copies out before returning. */
  pictureDataPtr(index: number): number;
  pictureDataCount(index: number): number;
  /** The full report, serialised as Metadata::toJson(). */
  reportJson(): string;
  delete(): void;
}

export interface AudModule {
  HEAPU8: Uint8Array;
  HEAPF32: Float32Array;
  HEAPF64: Float64Array;
  HEAPU32: Uint32Array;
  _malloc(bytes: number): number;
  _free(ptr: number): void;

  engineVersion(): string;
  engineBuildInfo(): EngineBuildInfo;
  selfTest(): SelfTestResult;

  DecodeSession: {
    create(probePtr: number, probeLength: number): RawDecodeSessionHandle | null;
  };

  Transport: {
    create(
      sourceRate: number,
      outputSampleRate: number,
      channels: number,
      ringCapacityFrames: number,
    ): RawTransportHandle | null;
  };

  Waveform: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). */
    create(audioBufferHandle: number): RawWaveformHandle | null;
  };

  Stft: {
    create(
      fftSize: number,
      hopSize: number,
      windowType: FftWindowType,
      scaling: FftSpectrumScaling,
      centered: boolean,
      sampleRate: number,
    ): RawStftHandle | null;
  };

  Spectrogram: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle() (or, inside the spectrogram
     *  worker, that worker's own AudioBuffer — see M07 "worker/PCM ownership"). `byteBudget` <= 0
     *  uses the engine default (128 MiB). */
    create(audioBufferHandle: number, byteBudget: number): RawSpectrogramHandle | null;
  };

  PcmBuffer: {
    create(sampleRate: number, channels: number): RawPcmBufferHandle | null;
  };

  Loudness: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `oversampling` must be 4, 8
     *  or 16 (0 defaults to 4, the BS.1770-4 Annex 2 spec-compliant minimum). */
    create(audioBufferHandle: number, oversampling: number): RawLoudnessHandle | null;
  };

  Statistics: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `containerBitDepth` is the
     *  source integer container's depth (0 for float sources) — see StreamInfo.bitDepth. */
    create(audioBufferHandle: number, containerBitDepth: number): RawStatisticsHandle | null;
  };

  Silence: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). The series pointers/counts
     *  come from a finished Statistics/Loudness handle's result (rmsSeriesPtr/rmsSeriesCount/
     *  rmsSeriesChannelCount, allZeroSeriesPtr/allZeroSeriesCount, momentaryPtr/momentaryCount) —
     *  pass 0/0 for any series that isn't available yet (that mode then reports no regions). */
    create(
      audioBufferHandle: number,
      rmsSeriesPtr: number,
      rmsSeriesCount: number,
      rmsChannelCount: number,
      allZeroSeriesPtr: number,
      allZeroSeriesCount: number,
      momentaryLufsPtr: number,
      momentaryLufsCount: number,
    ): RawSilenceHandle | null;
  };

  Clipping: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `containerBitDepth` is the
     *  source integer container's depth (0 for float sources) — see StreamInfo.bitDepth. Call
     *  `configure()` before the first processAvailableChunks() to set non-default parameters. */
    create(audioBufferHandle: number, containerBitDepth: number): RawClippingHandle | null;
  };

  Dc: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). Call `configure()` before the
     *  first processAvailableChunks() to use a non-default significance threshold. */
    create(audioBufferHandle: number): RawDcHandle | null;
  };

  Beats: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). Call `configure()` before the
     *  first processAvailableChunks() to use non-default parameters. */
    create(audioBufferHandle: number): RawBeatsHandle | null;
  };

  Transients: {
    /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `onsetTimesPtr`/
     *  `onsetStrengthsPtr` are HEAPF64/HEAPF32 arrays of length `onsetCount` — a finished Beats
     *  handle's `onsets` (timeSeconds, strength), the M13 candidates this analyser refines,
     *  classifies and augments with defect detection. Pass 0/0/0 for none (musical transients then
     *  come back empty; click/dropout defects still run). */
    create(
      audioBufferHandle: number,
      onsetTimesPtr: number,
      onsetStrengthsPtr: number,
      onsetCount: number,
    ): RawTransientsHandle | null;
  };

  Metadata: {
    /** `ptr`/`length` must be the *entire* file's bytes, not a probe slice (see M15: tag data can
     *  live arbitrarily far into the file). */
    create(ptr: number, length: number): RawMetadataHandle | null;
  };
}

export interface CreateAudModuleOptions {
  wasmUrl?: string;
}

export default function createAudModule(opts?: CreateAudModuleOptions): Promise<AudModule>;
