// Idiomatic TS wrapper over the raw Embind surface. `create()` is the only async entry point —
// everything after instantiation is synchronous from JS's point of view (M01).

import createAudModule from './aud_wasm.js';
import type {
  AudModule,
  CreateAudModuleOptions,
  DecodeDiagnostic,
  EngineBuildInfo,
  FftSpectrumScaling,
  FftWindowType,
  MetadataResult,
  OperationResult,
  RawDecodeSessionHandle,
  RawLoudnessHandle,
  RawMetadataHandle,
  RawPcmBufferHandle,
  RawSilenceHandle,
  RawSpectrogramHandle,
  RawStatisticsHandle,
  RawStftHandle,
  RawTransportHandle,
  RawTransportState,
  RawWaveformHandle,
  SilenceKind,
  SilencePosition,
  SpectrogramDecimation,
  SpectrogramFreqAxis,
  StreamInfo,
  WaveformChannelSelector,
} from './aud_wasm.d.ts';
import {
  copyFloat32IntoHeap,
  copyIntoHeap,
  copyPointerArrayIntoHeap,
  float32View,
  float64View,
  uint8View,
  uint32View,
  noteGrowthBoundary,
} from './heap_view.ts';
import { tileBytesView, overviewBytesView } from './spectrogramView.ts';
import { waveformBinsView, type WaveformBinsView } from './waveformView.ts';

// Dev-only leak backstop (M01 rule 4: "a FinalizationRegistry as a leak backstop that logs loudly
// in dev"). Never relied upon for correctness — dispose()/Symbol.dispose is.
const leakRegistry = new FinalizationRegistry<string>((label) => {
  if (import.meta.env?.DEV) {
    // eslint-disable-next-line no-console
    console.warn(`[audWASM] ${label} was garbage-collected without being disposed — leaked WASM memory`);
  }
});

export class DecodeSession implements Disposable {
  private handle: RawDecodeSessionHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawDecodeSessionHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'DecodeSession', this);
  }

  /** Sniffs `probeBytes` (>= 64KB recommended, per M02's detection ladder) and, on success,
   *  returns a session ready to feed(). Returns null if the format could not be identified. */
  static create(module: AudModule, probeBytes: Uint8Array): DecodeSession | null {
    const { ptr, length } = copyIntoHeap(module, probeBytes);
    try {
      const handle = module.DecodeSession.create(ptr, length);
      return handle ? new DecodeSession(module, handle) : null;
    } finally {
      module._free(ptr);
      noteGrowthBoundary();
    }
  }

  feed(bytes: Uint8Array): void {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    const { ptr, length } = copyIntoHeap(this.module, bytes);
    try {
      const result = this.handle.feedBytes(ptr, length);
      if (!result.ok) throw new Error(`decode failed: [${result.code}] ${result.detail ?? ''}`);
    } finally {
      this.module._free(ptr);
      noteGrowthBoundary();
    }
  }

  finish(): void {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    const result = this.handle.finish();
    noteGrowthBoundary();
    if (!result.ok) throw new Error(`decode failed at finish(): [${result.code}] ${result.detail ?? ''}`);
  }

  get streamInfo(): StreamInfo {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    return this.handle.getStreamInfo();
  }

  get decodedFrameCount(): number {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    return this.handle.getDecodedFrameCount();
  }

  get diagnostics(): DecodeDiagnostic[] {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    return this.handle.getDiagnostics();
  }

  /** Opaque handle for Transport.attachSource(). This DecodeSession must outlive the Transport's
   *  use of it — the underlying aud::AudioBuffer is non-owning on the Transport side. */
  get audioBufferHandle(): number {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    return this.handle.audioBufferHandle();
  }

  /** Copies out decoded PCM for one channel as a plain Float32Array — the spectrogram worker's own
   *  copy of the track (M07 "worker/PCM ownership": no SharedArrayBuffer available, so the worker
   *  can't share this session's AudioBuffer directly). Not chunked/streaming: call once per
   *  channel after decode completes. */
  readChannelPcm(channel: number, beginFrame: number, endFrame: number): Float32Array {
    if (!this.handle) throw new Error('DecodeSession used after dispose()');
    const frames = Math.max(0, endFrame - beginFrame);
    const ptr = this.module._malloc(frames * 4);
    try {
      const result = this.handle.readPlanarChannel(channel, beginFrame, endFrame, ptr);
      if (!result.ok) throw new Error(`readChannelPcm failed: [${result.code}] ${result.detail ?? ''}`);
      return float32View(this.module, ptr, frames).slice();
    } finally {
      this.module._free(ptr);
      noteGrowthBoundary();
    }
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

/** Idiomatic wrapper over the raw Embind Transport surface (playback_bindings.cpp). Illegal
 *  transitions return `{ ok: false }` rather than throwing — unlike DecodeSession's feed()/finish(),
 *  a rejected transport transition (e.g. double-clicking play) is routine UI traffic, not a fatal
 *  decode error, so callers decide what to do with it rather than having it thrown at them. */
export class Transport implements Disposable {
  private handle: RawTransportHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawTransportHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Transport', this);
  }

  /** `channels` must match the decode session's stream info; `ringCapacityFrames` should cover a
   *  few hundred ms of output-rate audio (M03 risk table: "prebuffer >= 250ms"). */
  static create(
    module: AudModule,
    sourceRate: number,
    outputSampleRate: number,
    channels: number,
    ringCapacityFrames: number,
  ): Transport | null {
    const handle = module.Transport.create(sourceRate, outputSampleRate, channels, ringCapacityFrames);
    return handle ? new Transport(module, handle) : null;
  }

  private get raw(): RawTransportHandle {
    if (!this.handle) throw new Error('Transport used after dispose()');
    return this.handle;
  }

  /** `audioBufferHandle` comes from DecodeSession's (raw handle's) `audioBufferHandle()`. The
   *  DecodeSession must outlive this Transport's use of it — the buffer is non-owning here. */
  attachSource(audioBufferHandle: number): void {
    this.raw.attachSource(audioBufferHandle);
  }

  setSourceComplete(complete: boolean): void {
    this.raw.setSourceComplete(complete);
  }

  load(): OperationResult { return this.raw.dispatchLoad(); }
  ready(durationFrames: number): OperationResult { return this.raw.dispatchReady(durationFrames); }
  play(): OperationResult { return this.raw.dispatchPlay(); }
  pause(): OperationResult { return this.raw.dispatchPause(); }
  seekTo(targetFrame: number): OperationResult { return this.raw.dispatchSeekTo(targetFrame); }
  setLoopRange(beginFrame: number, endFrame: number): OperationResult {
    return this.raw.dispatchSetLoopRange(beginFrame, endFrame);
  }
  setLoopEnabled(enabled: boolean): OperationResult { return this.raw.dispatchSetLoopEnabled(enabled); }
  setLoopCrossfadeFrames(frames: number): OperationResult {
    return this.raw.dispatchSetLoopCrossfadeFrames(frames);
  }
  setGain(gain: number): OperationResult { return this.raw.dispatchSetGain(gain); }
  reset(): OperationResult { return this.raw.dispatchReset(); }

  /** Pulls up to `maxFrames` resampled frames from the source into the internal ring. Call
   *  whenever the producer context has idle time, to stay ahead of the audio thread's demand. */
  pump(maxFrames: number): number {
    return this.raw.pump(maxFrames);
  }

  /** Reads up to `framesRequested` frames out of the ring (gain-applied), returned as one
   *  Float32Array per channel. A shorter-than-requested result is an underrun — the caller must
   *  silence-fill the remainder on its side and count a dropout (M03 risk table). */
  render(framesRequested: number, channels: number): Float32Array[] {
    const bytesPerChannel = framesRequested * 4;
    const ptr = this.module._malloc(bytesPerChannel * channels);
    try {
      const got = this.raw.renderInto(ptr, framesRequested);
      noteGrowthBoundary();
      const out: Float32Array[] = [];
      for (let ch = 0; ch < channels; ch++) {
        const view = float32View(this.module, ptr + ch * bytesPerChannel, framesRequested);
        out.push(view.slice()); // copy out — the heap region is freed immediately below
      }
      void got; // shortfall is implicit: trailing samples in `out` are already silence-filled natively
      return out;
    } finally {
      this.module._free(ptr);
      noteGrowthBoundary();
    }
  }

  get state(): RawTransportState {
    return this.raw.getState();
  }

  get ringFramesAvailable(): number {
    return this.raw.ringFramesAvailable();
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

/** Idiomatic wrapper over the raw Embind Waveform surface (waveform_bindings.cpp). Call
 *  processAvailableChunks() after each feed() on the DecodeSession that owns `audioBufferHandle`
 *  (and finish() after that session's finish()) to keep the waveform progressing alongside
 *  decode — M04 "Streaming generation". */
export class Waveform implements Disposable {
  private handle: RawWaveformHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawWaveformHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Waveform', this);
  }

  /** `audioBufferHandle` comes from DecodeSession.audioBufferHandle. The DecodeSession must
   *  outlive this Waveform's use of it — the underlying aud::AudioBuffer is non-owning here. */
  static create(module: AudModule, audioBufferHandle: number): Waveform | null {
    const handle = module.Waveform.create(audioBufferHandle);
    return handle ? new Waveform(module, handle) : null;
  }

  private get raw(): RawWaveformHandle {
    if (!this.handle) throw new Error('Waveform used after dispose()');
    return this.handle;
  }

  processAvailableChunks(): void {
    const result = this.raw.processAvailableChunks();
    if (!result.ok) throw new Error(`waveform reduction failed: [${result.code}] ${result.detail ?? ''}`);
  }

  finish(): void {
    const result = this.raw.finish();
    if (!result.ok) throw new Error(`waveform finish failed: [${result.code}] ${result.detail ?? ''}`);
  }

  get channelCount(): number {
    return this.raw.channelCount();
  }

  get isComplete(): boolean {
    return this.raw.isComplete();
  }

  /** Raw (unaggregated) level-0 bins for one channel — mostly useful for tests/inspection; the
   *  renderer (M17) should prefer query() for pixel-width-sized data. */
  levelZeroBins(channel: number): WaveformBinsView {
    const result = this.raw.levelZeroBins(channel);
    if (!result.ok || result.ptr === undefined || result.binCount === undefined) {
      throw new Error(`levelZeroBins failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return waveformBinsView(this.module, result.ptr, result.binCount);
  }

  monoSumBins(): WaveformBinsView {
    return this.derivedBins(this.raw.monoSumBins());
  }

  midBins(): WaveformBinsView {
    return this.derivedBins(this.raw.midBins());
  }

  sideBins(): WaveformBinsView {
    return this.derivedBins(this.raw.sideBins());
  }

  private derivedBins(result: ReturnType<RawWaveformHandle['monoSumBins']>): WaveformBinsView {
    if (!result.ok || result.ptr === undefined || result.binCount === undefined) {
      throw new Error(`waveform derived-bins request failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return waveformBinsView(this.module, result.ptr, result.binCount);
  }

  /** Aggregates bins down to `binCount` output bins covering [rangeBeginFrame, rangeEndFrame) —
   *  the shape the M17 renderer actually wants (~pixel-width bins over the visible range). */
  query(
    channelsMode: WaveformChannelSelector,
    rangeBeginFrame: number,
    rangeEndFrame: number,
    binCount: number,
  ): { bins: WaveformBinsView; channels: number; framesPerBin: number; isComplete: boolean; isRawPcm: boolean } {
    const result = this.raw.query(channelsMode, rangeBeginFrame, rangeEndFrame, binCount);
    if (
      !result.ok ||
      result.ptr === undefined ||
      result.channels === undefined ||
      result.binCount === undefined ||
      result.framesPerBin === undefined ||
      result.isComplete === undefined
    ) {
      throw new Error(`waveform query failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return {
      bins: waveformBinsView(this.module, result.ptr, result.channels * result.binCount),
      channels: result.channels,
      framesPerBin: result.framesPerBin,
      isComplete: result.isComplete,
      isRawPcm: result.isRawPcm ?? false,
    };
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

/** Idiomatic wrapper over the raw Embind Stft surface (fft_bindings.cpp, M06). Batches every frame
 *  completed by process()/finish() into one contiguous heap buffer — never the whole track's STFT
 *  at once (M06's "never materialise the whole STFT" rule). Had no TS surface before M07; added
 *  alongside Spectrogram/PcmBuffer rather than left as a second gap. */
export class Stft implements Disposable {
  private handle: RawStftHandle | null;
  private readonly module: AudModule;
  private readonly binCountCached: number;

  private constructor(module: AudModule, handle: RawStftHandle) {
    this.module = module;
    this.handle = handle;
    this.binCountCached = handle.binCount();
    leakRegistry.register(this, 'Stft', this);
  }

  static create(
    module: AudModule,
    fftSize: number,
    hopSize: number,
    windowType: FftWindowType,
    scaling: FftSpectrumScaling,
    centered: boolean,
    sampleRate: number,
  ): Stft | null {
    const handle = module.Stft.create(fftSize, hopSize, windowType, scaling, centered, sampleRate);
    return handle ? new Stft(module, handle) : null;
  }

  private get raw(): RawStftHandle {
    if (!this.handle) throw new Error('Stft used after dispose()');
    return this.handle;
  }

  get binCount(): number {
    return this.binCountCached;
  }

  frameTimeSeconds(frameIndex: number): number {
    return this.raw.frameTimeSeconds(frameIndex);
  }

  binFrequencyHz(bin: number): number {
    return this.raw.binFrequencyHz(bin);
  }

  frameCountFor(totalFrames: number): number {
    return this.raw.frameCountFor(totalFrames);
  }

  /** `samples` is one channel's planar float32 samples; copied into the heap and freed
   *  immediately. Returns the frames completed by this call as [frameCount][binCount] magnitudes. */
  process(samples: Float32Array): Float32Array[] {
    const { ptr, length } = copyFloat32IntoHeap(this.module, samples);
    try {
      const result = this.raw.process(ptr, length);
      return this.framesFromResult(result, 'process');
    } finally {
      this.module._free(ptr);
      noteGrowthBoundary();
    }
  }

  finish(): Float32Array[] {
    const result = this.raw.finish();
    return this.framesFromResult(result, 'finish');
  }

  private framesFromResult(result: ReturnType<RawStftHandle['process']>, callName: string): Float32Array[] {
    if (!result.ok || result.ptr === undefined || result.binCount === undefined || result.frameCount === undefined) {
      throw new Error(`Stft.${callName} failed: [${result.code}] ${result.detail ?? ''}`);
    }
    const flat = float32View(this.module, result.ptr, result.binCount * result.frameCount);
    const frames: Float32Array[] = [];
    for (let f = 0; f < result.frameCount; f++) {
      frames.push(flat.slice(f * result.binCount, (f + 1) * result.binCount));
    }
    return frames;
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

/** Idiomatic wrapper over the raw Embind PcmBuffer surface (spectrogram_bindings.cpp). Builds a
 *  standalone aud::AudioBuffer from planar PCM copied into the heap, independent of DecodeSession —
 *  the spectrogram worker's own copy of the decoded track (M07 "worker/PCM ownership" decision). */
export class PcmBuffer implements Disposable {
  private handle: RawPcmBufferHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawPcmBufferHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'PcmBuffer', this);
  }

  static create(module: AudModule, sampleRate: number, channels: number): PcmBuffer | null {
    const handle = module.PcmBuffer.create(sampleRate, channels);
    return handle ? new PcmBuffer(module, handle) : null;
  }

  private get raw(): RawPcmBufferHandle {
    if (!this.handle) throw new Error('PcmBuffer used after dispose()');
    return this.handle;
  }

  /** `channels` is one Float32Array per channel, each the same length. Copies every channel into
   *  the heap plus a pointer-array over them, appends, then frees both — not chunked/streaming
   *  (M07 scope decision: the worker builds its whole copy once, right after decode completes, not
   *  incrementally alongside it). */
  appendPlanar(channels: readonly Float32Array[]): void {
    if (channels.length === 0) return;
    const frames = channels[0]!.length;
    const heapCopies = channels.map((c) => copyFloat32IntoHeap(this.module, c));
    const ptrArray = copyPointerArrayIntoHeap(
      this.module,
      heapCopies.map((c) => c.ptr),
    );
    try {
      const result = this.raw.appendPlanar(ptrArray, channels.length, frames);
      if (!result.ok) throw new Error(`PcmBuffer.appendPlanar failed: [${result.code}] ${result.detail ?? ''}`);
    } finally {
      for (const c of heapCopies) this.module._free(c.ptr);
      this.module._free(ptrArray);
      noteGrowthBoundary();
    }
  }

  /** Opaque handle for Spectrogram.create(). This PcmBuffer must outlive the Spectrogram's use of
   *  it — the underlying aud::AudioBuffer is non-owning on the Spectrogram side. */
  get audioBufferHandle(): number {
    return this.raw.audioBufferHandle();
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

export interface SpectrogramTile {
  bytes: Uint8Array;  // kTileWidth*kTileHeight, row 0 = lowest displayed frequency
  floorDb: number;
  ceilDb: number;
}

export interface SpectrogramOverview {
  bytes: Uint8Array;  // width*height
  width: number;
  height: number;
  floorDb: number;
  ceilDb: number;
}

export interface SpectrogramPoint {
  frequencyHz: number;
  magnitudeDb: number;
}

/** Idiomatic wrapper over the raw Embind Spectrogram surface (M07). Meant to be constructed inside
 *  the dedicated spectrogram worker against a PcmBuffer's audioBufferHandle — see
 *  frontend/workers/spectrogramWorker.ts. */
export class Spectrogram implements Disposable {
  private handle: RawSpectrogramHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawSpectrogramHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Spectrogram', this);
  }

  /** `byteBudget` <= 0 uses the engine default (128 MiB). */
  static create(module: AudModule, audioBufferHandle: number, byteBudget = 0): Spectrogram | null {
    const handle = module.Spectrogram.create(audioBufferHandle, byteBudget);
    return handle ? new Spectrogram(module, handle) : null;
  }

  private get raw(): RawSpectrogramHandle {
    if (!this.handle) throw new Error('Spectrogram used after dispose()');
    return this.handle;
  }

  /** Swaps in a new generation config; returns the new configHash callers must stamp into every
   *  TileKey they request from here on (M07: old-hash tiles are simply never regenerated, they age
   *  out of the cache normally — "old tiles shown until new ones arrive"). */
  setConfig(
    fftSize: number,
    window: FftWindowType,
    scaling: FftSpectrumScaling,
    freqAxis: SpectrogramFreqAxis,
    decimation: SpectrogramDecimation,
    minHz: number,
    floorDb: number,
    ceilDb: number,
  ): number {
    const result = this.raw.setConfig(fftSize, window, scaling, freqAxis, decimation, minHz, floorDb, ceilDb);
    if (!result.ok || result.configHash === undefined) {
      throw new Error(`Spectrogram.setConfig failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return result.configHash;
  }

  get currentConfigHash(): number {
    return this.raw.currentConfigHash();
  }

  /** Generates (or returns cached) tile bytes, copied out of the heap — safe to hold and transfer
   *  to another thread, unlike a raw heap view. */
  requestTile(level: number, tileX: number, channel: number): SpectrogramTile {
    const result = this.raw.requestTile(level, tileX, channel);
    if (!result.ok || result.ptr === undefined || result.byteLength === undefined ||
        result.floorDb === undefined || result.ceilDb === undefined) {
      throw new Error(`Spectrogram.requestTile failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return {
      bytes: tileBytesView(this.module, result.ptr, result.byteLength).slice(),
      floorDb: result.floorDb,
      ceilDb: result.ceilDb,
    };
  }

  invalidateConfig(staleConfigHash: number): void {
    this.raw.invalidateConfig(staleConfigHash);
  }

  get cacheTileCount(): number {
    return this.raw.cacheTileCount();
  }

  get cacheCurrentBytes(): number {
    return this.raw.cacheCurrentBytes();
  }

  get cacheByteBudget(): number {
    return this.raw.cacheByteBudget();
  }

  overview(channel: number): SpectrogramOverview {
    const result = this.raw.overview(channel);
    if (!result.ok || result.ptr === undefined || result.width === undefined || result.height === undefined ||
        result.floorDb === undefined || result.ceilDb === undefined) {
      throw new Error(`Spectrogram.overview failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return {
      bytes: overviewBytesView(this.module, result.ptr, result.width, result.height).slice(),
      width: result.width,
      height: result.height,
      floorDb: result.floorDb,
      ceilDb: result.ceilDb,
    };
  }

  /** Exact cursor-readout query (M07) — bypasses the quantised tile cache entirely. */
  queryPoint(channel: number, timeSeconds: number, targetHz: number): SpectrogramPoint {
    const result = this.raw.queryPoint(channel, timeSeconds, targetHz);
    if (!result.ok || result.frequencyHz === undefined || result.magnitudeDb === undefined) {
      throw new Error(`Spectrogram.queryPoint failed: [${result.code}] ${result.detail ?? ''}`);
    }
    return { frequencyHz: result.frequencyHz, magnitudeDb: result.magnitudeDb };
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

export interface LoudnessResult {
  integratedLufs: number;
  loudnessRangeLu: number;
  truePeakDbtp: number;
  samplePeakDbfs: number;
  truePeakFrame: number;
  truePeakOversampling: number;
  usedFallbackChannelLayout: boolean;
  truePeakPerChannelDbtp: Float64Array;
  samplePeakPerChannelDbfs: Float64Array;
  /** 100 ms-resolution momentary loudness, in LUFS; empty until 400 ms of programme has landed. */
  momentaryLufs: Float32Array;
  /** 100 ms-resolution short-term loudness, in LUFS; empty until 3 s of programme has landed. */
  shortTermLufs: Float32Array;
}

/** Idiomatic wrapper over the raw Embind Loudness surface (loudness_bindings.cpp, M08). Driven
 *  directly against a DecodeSession's audioBufferHandle(), same processAvailableChunks()/finish()
 *  polling contract as Waveform — see waveform_bindings.cpp's comment for why this bypasses the
 *  Analyzer interface at the binding layer (M20's shared dispatch isn't built yet). */
export class Loudness implements Disposable {
  private handle: RawLoudnessHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawLoudnessHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Loudness', this);
  }

  /** `oversampling` must be 4, 8 or 16 (default 4, the BS.1770-4 Annex 2 spec-compliant minimum;
   *  8/16 are the "high precision" true-peak modes — see M08's true_peak.hpp). */
  static create(module: AudModule, audioBufferHandle: number, oversampling = 4): Loudness | null {
    const handle = module.Loudness.create(audioBufferHandle, oversampling);
    return handle ? new Loudness(module, handle) : null;
  }

  private get raw(): RawLoudnessHandle {
    if (!this.handle) throw new Error('Loudness used after dispose()');
    return this.handle;
  }

  /** Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
   *  alongside progressive decode. */
  processAvailableChunks(): void {
    const result = this.raw.processAvailableChunks();
    if (!result.ok) throw new Error(`Loudness.processAvailableChunks failed: [${result.code}] ${result.detail ?? ''}`);
  }

  /** Runs the two-stage integrated-loudness gate and the LRA percentile method; call once after
   *  decode is complete. Copies the per-channel/time-series heap views out (see float64View's/
   *  float32View's growth-boundary caveat) so the result is safe to hold past the next allocation. */
  finish(): LoudnessResult {
    const r = this.raw.finish();
    if (
      !r.ok ||
      r.integratedLufs === undefined ||
      r.loudnessRangeLu === undefined ||
      r.truePeakDbtp === undefined ||
      r.samplePeakDbfs === undefined ||
      r.truePeakFrame === undefined ||
      r.truePeakOversampling === undefined ||
      r.usedFallbackChannelLayout === undefined ||
      r.truePeakPerChannelPtr === undefined ||
      r.truePeakPerChannelCount === undefined ||
      r.samplePeakPerChannelPtr === undefined ||
      r.samplePeakPerChannelCount === undefined ||
      r.momentaryPtr === undefined ||
      r.momentaryCount === undefined ||
      r.shortTermPtr === undefined ||
      r.shortTermCount === undefined
    ) {
      throw new Error(`Loudness.finish failed: [${r.code}] ${r.detail ?? ''}`);
    }
    return {
      integratedLufs: r.integratedLufs,
      loudnessRangeLu: r.loudnessRangeLu,
      truePeakDbtp: r.truePeakDbtp,
      samplePeakDbfs: r.samplePeakDbfs,
      truePeakFrame: r.truePeakFrame,
      truePeakOversampling: r.truePeakOversampling,
      usedFallbackChannelLayout: r.usedFallbackChannelLayout,
      truePeakPerChannelDbtp: float64View(this.module, r.truePeakPerChannelPtr, r.truePeakPerChannelCount).slice(),
      samplePeakPerChannelDbfs: float64View(this.module, r.samplePeakPerChannelPtr, r.samplePeakPerChannelCount).slice(),
      momentaryLufs: float32View(this.module, r.momentaryPtr, r.momentaryCount).slice(),
      shortTermLufs: float32View(this.module, r.shortTermPtr, r.shortTermCount).slice(),
    };
  }

  /** Gain (dB) to reach `targetLufs` (e.g. -14 Spotify/YouTube/Amazon, -23 EBU R128 broadcast) —
   *  information only, never applied automatically. Only meaningful after finish(). */
  gainToTargetDb(targetLufs: number): number {
    return this.raw.gainToTargetDb(targetLufs);
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

export interface StatisticsChannel {
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
  /** 1024-bucket amplitude histogram, symmetric around zero, linear range [-1, 1]. */
  histogram: Uint32Array;
}

export interface StatisticsStereo {
  correlation: number;
  balanceDb: number;
  monoCompatibilityDb: number;
  /** 50 ms-resolution correlation, for localising phase problems in time. */
  correlationSeries: Float32Array;
}

export interface StatisticsResult {
  sampleRate: number;
  channelCount: number;
  frameCount: number;
  crestFactorDb: number;
  dynamicRangeDr: number;
  channels: StatisticsChannel[];
  stereo: StatisticsStereo | null;
  /** 50 ms-resolution linear RMS, interleaved by channel: [ch0_w0, ch1_w0, ch0_w1, ch1_w1, ...]. */
  rmsSeries: Float32Array;
  rmsSeriesChannelCount: number;
  /** Same interleaving/grid as rmsSeries: 1 if every sample in that window was exactly zero. Feed
   *  this straight into AudioEngine.createSilence() for M10's digital-silence mode. */
  allZeroSeries: Uint8Array;
  /** The report exactly as docs/report-schema.json defines it — the stable, versioned artifact
   *  M09 decided to expose directly (scriptable, copy-paste into a bug report). */
  reportJson: string;
}

/** Idiomatic wrapper over the raw Embind Statistics surface (statistics_bindings.cpp, M09). Same
 *  processAvailableChunks()/finish() polling contract as Waveform/Loudness — see
 *  loudness_analyzer.hpp's comment for why this bypasses the Analyzer registry at the binding
 *  layer (M20's shared dispatch isn't built yet). */
export class Statistics implements Disposable {
  private handle: RawStatisticsHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawStatisticsHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Statistics', this);
  }

  /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `containerBitDepth` is the
   *  source integer container's depth (0 for float sources) — see StreamInfo.bitDepth. */
  static create(module: AudModule, audioBufferHandle: number, containerBitDepth: number): Statistics | null {
    const handle = module.Statistics.create(audioBufferHandle, containerBitDepth);
    return handle ? new Statistics(module, handle) : null;
  }

  private get raw(): RawStatisticsHandle {
    if (!this.handle) throw new Error('Statistics used after dispose()');
    return this.handle;
  }

  /** Feeds every AudioBuffer chunk appended since the last call — safe to call repeatedly
   *  alongside progressive decode. */
  processAvailableChunks(): void {
    const result = this.raw.processAvailableChunks();
    if (!result.ok) throw new Error(`Statistics.processAvailableChunks failed: [${result.code}] ${result.detail ?? ''}`);
  }

  /** Runs the finish() pass (bit depth, dynamic range, stereo correlation); call once after decode
   *  is complete. Copies every heap view out (see float32View's/uint32View's growth-boundary
   *  caveat) so the result is safe to hold past the next allocation. */
  finish(): StatisticsResult {
    const r = this.raw.finish();
    if (
      !r.ok ||
      r.sampleRate === undefined ||
      r.channelCount === undefined ||
      r.frameCount === undefined ||
      r.crestFactorDb === undefined ||
      r.dynamicRangeDr === undefined ||
      r.reportJson === undefined ||
      r.channels === undefined ||
      r.stereo === undefined ||
      r.rmsSeriesPtr === undefined ||
      r.rmsSeriesCount === undefined ||
      r.rmsSeriesChannelCount === undefined ||
      r.allZeroSeriesPtr === undefined ||
      r.allZeroSeriesCount === undefined
    ) {
      throw new Error(`Statistics.finish failed: [${r.code}] ${r.detail ?? ''}`);
    }

    const channels: StatisticsChannel[] = r.channels.map((c) => ({
      peak: c.peak,
      peakDbfs: c.peakDbfs,
      peakFrame: c.peakFrame,
      minValue: c.minValue,
      maxValue: c.maxValue,
      dcOffset: c.dcOffset,
      rms: c.rms,
      rmsDbfs: c.rmsDbfs,
      variance: c.variance,
      stdDev: c.stdDev,
      crestFactorDb: c.crestFactorDb,
      zeroCrossingRate: c.zeroCrossingRate,
      effectiveBitDepth: c.effectiveBitDepth,
      containerBitDepth: c.containerBitDepth,
      ditherLikely: c.ditherLikely,
      ditherConfidence: c.ditherConfidence,
      bitDepthDescription: c.bitDepthDescription,
      histogram: uint32View(this.module, c.histogramPtr, c.histogramCount).slice(),
    }));

    const stereo: StatisticsStereo | null = r.stereo
      ? {
          correlation: r.stereo.correlation,
          balanceDb: r.stereo.balanceDb,
          monoCompatibilityDb: r.stereo.monoCompatibilityDb,
          correlationSeries: float32View(this.module, r.stereo.correlationSeriesPtr, r.stereo.correlationSeriesCount).slice(),
        }
      : null;

    return {
      sampleRate: r.sampleRate,
      channelCount: r.channelCount,
      frameCount: r.frameCount,
      crestFactorDb: r.crestFactorDb,
      dynamicRangeDr: r.dynamicRangeDr,
      channels,
      stereo,
      rmsSeries: float32View(this.module, r.rmsSeriesPtr, r.rmsSeriesCount).slice(),
      rmsSeriesChannelCount: r.rmsSeriesChannelCount,
      allZeroSeries: uint8View(this.module, r.allZeroSeriesPtr, r.allZeroSeriesCount).slice(),
      reportJson: r.reportJson,
    };
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

export interface SilenceRegion {
  beginFrame: number;
  endFrame: number;
  startSeconds: number;
  endSeconds: number;
  kind: SilenceKind;
  position: SilencePosition;
  /** Coarse (window-average) until refineThreshold()/refineDigital() has run for this region's
   *  kind; sample-accurate afterwards — see M10 "Boundary refinement". */
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

export interface SilenceModeResult {
  regions: SilenceRegion[];
  leadingSilenceSeconds: number;
  trailingSilenceSeconds: number;
  totalSilenceSeconds: number;
  silenceFraction: number;
  /** Echoed back — a region list is meaningless without the parameters that produced it (M10). */
  parametersUsed: SilenceParametersUsed;
}

export interface SilenceDetectResult {
  threshold: SilenceModeResult;
  digital: SilenceModeResult;
  perceptual: SilenceModeResult;
}

export interface SilenceDetectParams {
  thresholdDb?: number;
  minDurationMs?: number;
  mergeGapMs?: number;
  channelModeAny?: boolean;
  useHysteresis?: boolean;
  hysteresisDb?: number;
  /** R128 absolute gate, -70 LUFS by default (M08). */
  perceptualGateLufs?: number;
}

/** Idiomatic wrapper over the raw Embind Silence surface (silence_bindings.cpp, M10). Unlike
 *  Waveform/Loudness/Statistics, detect() does not touch PCM at all — it's a pure, instant
 *  recompute over the M09 RMS/digital-silence series and M08 momentary-loudness series copied in
 *  at create() time, safe to call on every threshold-slider tick (M10 "reparameterisation
 *  recomputes ... instantly, on the main thread"). refineThreshold()/refineDigital() are the
 *  separate, deliberately PCM-touching step — call those once, debounced, after the user stops
 *  dragging, and only after at least one detect() call. */
export class Silence implements Disposable {
  private handle: RawSilenceHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawSilenceHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Silence', this);
  }

  /** `audioBufferHandle` is a DecodeSession's audioBufferHandle(). `rmsSeries`/
   *  `rmsSeriesChannelCount`/`allZeroSeries` come from a finished Statistics.finish(); `momentaryLufs`
   *  (optional — omit if no Loudness pass has run yet) comes from a finished Loudness.finish() and
   *  enables perceptual-mode detection. */
  static create(
    module: AudModule,
    audioBufferHandle: number,
    rmsSeries: Float32Array,
    rmsSeriesChannelCount: number,
    allZeroSeries: Uint8Array,
    momentaryLufs?: Float32Array,
  ): Silence | null {
    const rms = copyFloat32IntoHeap(module, rmsSeries);
    const allZero = copyIntoHeap(module, allZeroSeries);
    const momentary = momentaryLufs ? copyFloat32IntoHeap(module, momentaryLufs) : { ptr: 0, length: 0 };
    noteGrowthBoundary();

    const handle = module.Silence.create(
      audioBufferHandle,
      rms.ptr,
      rms.length,
      rmsSeriesChannelCount,
      allZero.ptr,
      allZero.length,
      momentary.ptr,
      momentary.length,
    );

    // SilenceHandle::create() copies these synchronously into its own SilenceInput — free
    // immediately, same discipline as copyIntoHeap's own doc comment.
    module._free(rms.ptr);
    module._free(allZero.ptr);
    if (momentary.ptr !== 0) module._free(momentary.ptr);

    return handle ? new Silence(module, handle) : null;
  }

  private get raw(): RawSilenceHandle {
    if (!this.handle) throw new Error('Silence used after dispose()');
    return this.handle;
  }

  private static toModeResult(r: {
    ok: boolean;
    code?: string;
    detail?: string;
    regions?: SilenceRegion[];
    leadingSilenceSeconds?: number;
    trailingSilenceSeconds?: number;
    totalSilenceSeconds?: number;
    silenceFraction?: number;
    parametersUsed?: SilenceParametersUsed;
  }): SilenceModeResult {
    if (
      !r.ok ||
      r.regions === undefined ||
      r.leadingSilenceSeconds === undefined ||
      r.trailingSilenceSeconds === undefined ||
      r.totalSilenceSeconds === undefined ||
      r.silenceFraction === undefined ||
      r.parametersUsed === undefined
    ) {
      throw new Error(`Silence mode result failed: [${r.code}] ${r.detail ?? ''}`);
    }
    return {
      regions: r.regions,
      leadingSilenceSeconds: r.leadingSilenceSeconds,
      trailingSilenceSeconds: r.trailingSilenceSeconds,
      totalSilenceSeconds: r.totalSilenceSeconds,
      silenceFraction: r.silenceFraction,
      parametersUsed: r.parametersUsed,
    };
  }

  /** O(windows), instant — no PCM access. Runs all three modes (threshold/digital/perceptual) and
   *  returns window-grid-precision regions; call refineThreshold()/refineDigital() afterwards
   *  (debounced) for sample-accurate boundaries. */
  detect(params: SilenceDetectParams = {}): SilenceDetectResult {
    const r = this.raw.detect(
      params.thresholdDb ?? -60,
      params.minDurationMs ?? 500,
      params.mergeGapMs ?? 100,
      params.channelModeAny ?? false,
      params.useHysteresis ?? true,
      params.hysteresisDb ?? 3,
      params.perceptualGateLufs ?? -70,
    );
    if (!r.ok || r.threshold === undefined || r.digital === undefined || r.perceptual === undefined) {
      throw new Error(`Silence.detect failed: [${r.code}] ${r.detail ?? ''}`);
    }
    return {
      threshold: Silence.toModeResult(r.threshold),
      digital: Silence.toModeResult(r.digital),
      perceptual: Silence.toModeResult(r.perceptual),
    };
  }

  /** Sample-precise boundaries + exact level stats for the threshold regions from the most recent
   *  detect() call. PCM-touching — call once, debounced, after the user stops dragging. */
  refineThreshold(): SilenceModeResult {
    return Silence.toModeResult(this.raw.refineThreshold());
  }

  /** Same, for the digital-silence regions from the most recent detect() call. */
  refineDigital(): SilenceModeResult {
    return Silence.toModeResult(this.raw.refineDigital());
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

/** Idiomatic wrapper over the raw Embind Metadata surface (metadata_bindings.cpp). One-shot, not a
 *  streaming Analyzer — see M15: metadata is read from the raw encoded file bytes directly, closer
 *  in shape to DecodeSession than to Loudness/Statistics/etc. */
export class Metadata implements Disposable {
  private handle: RawMetadataHandle | null;
  private readonly module: AudModule;

  private constructor(module: AudModule, handle: RawMetadataHandle) {
    this.module = module;
    this.handle = handle;
    leakRegistry.register(this, 'Metadata', this);
  }

  /** Extracts every tag M15 recognises from a *complete* copy of the file's bytes. Unlike
   *  DecodeSession.create()'s probe slice, tag parsing can need to see arbitrarily far into the
   *  file (a trailing ID3v1 tag, oversized cover art, MP4 atoms placed after `mdat`) — pass the
   *  whole file, not a prefix. Returns null only if construction itself failed; extract() is
   *  designed to never fail on its own (an untagged/garbage file is a valid empty result, not a
   *  null one — M15's "no tags at all" acceptance criterion). */
  static create(module: AudModule, fileBytes: Uint8Array): Metadata | null {
    const { ptr, length } = copyIntoHeap(module, fileBytes);
    try {
      const handle = module.Metadata.create(ptr, length);
      return handle ? new Metadata(module, handle) : null;
    } finally {
      module._free(ptr);
      noteGrowthBoundary();
    }
  }

  get result(): MetadataResult {
    if (!this.handle) throw new Error('Metadata used after dispose()');
    return this.handle.result();
  }

  /** The full report, serialised as Metadata::toJson() — the same shape a CLI/QA tool would
   *  consume, independent of this wrapper's own object shape. */
  get reportJson(): string {
    if (!this.handle) throw new Error('Metadata used after dispose()');
    return this.handle.reportJson();
  }

  /** Materialises picture `index`'s raw bytes as an owned Uint8Array (safe to keep after this
   *  Metadata is disposed) — pass to `createImageBitmap`/a Blob URL. M15 deliberately never decodes
   *  image formats in C++: the browser already has a hardened, fast decoder, and adding one (e.g.
   *  stb_image) would add both binary size and attack surface for no benefit. */
  getPictureBytes(index: number): Uint8Array {
    if (!this.handle) throw new Error('Metadata used after dispose()');
    const ptr = this.handle.pictureDataPtr(index);
    const count = this.handle.pictureDataCount(index);
    if (ptr === 0 || count === 0) return new Uint8Array(0);
    return uint8View(this.module, ptr, count).slice();
  }

  dispose(): void {
    this.handle?.delete();
    this.handle = null;
    leakRegistry.unregister(this);
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

export class AudioEngine implements Disposable {
  private constructor(private readonly module: AudModule) {}

  static async create(opts?: CreateAudModuleOptions): Promise<AudioEngine> {
    const module = await createAudModule(opts);
    return new AudioEngine(module);
  }

  get buildInfo(): EngineBuildInfo {
    return this.module.engineBuildInfo();
  }

  get version(): string {
    return this.module.engineVersion();
  }

  runSelfTest(): boolean {
    return this.module.selfTest().pass;
  }

  createDecodeSession(probeBytes: Uint8Array): DecodeSession | null {
    return DecodeSession.create(this.module, probeBytes);
  }

  createTransport(
    sourceRate: number,
    outputSampleRate: number,
    channels: number,
    ringCapacityFrames: number,
  ): Transport | null {
    return Transport.create(this.module, sourceRate, outputSampleRate, channels, ringCapacityFrames);
  }

  createWaveform(audioBufferHandle: number): Waveform | null {
    return Waveform.create(this.module, audioBufferHandle);
  }

  createStft(
    fftSize: number,
    hopSize: number,
    windowType: FftWindowType,
    scaling: FftSpectrumScaling,
    centered: boolean,
    sampleRate: number,
  ): Stft | null {
    return Stft.create(this.module, fftSize, hopSize, windowType, scaling, centered, sampleRate);
  }

  createPcmBuffer(sampleRate: number, channels: number): PcmBuffer | null {
    return PcmBuffer.create(this.module, sampleRate, channels);
  }

  createSpectrogram(audioBufferHandle: number, byteBudget = 0): Spectrogram | null {
    return Spectrogram.create(this.module, audioBufferHandle, byteBudget);
  }

  createLoudness(audioBufferHandle: number, oversampling = 4): Loudness | null {
    return Loudness.create(this.module, audioBufferHandle, oversampling);
  }

  /** `containerBitDepth` is the source integer container's depth (0 for float sources) — see
   *  DecodeSession.streamInfo.bitDepth. */
  createStatistics(audioBufferHandle: number, containerBitDepth = 0): Statistics | null {
    return Statistics.create(this.module, audioBufferHandle, containerBitDepth);
  }

  /** `rmsSeries`/`rmsSeriesChannelCount`/`allZeroSeries` come from a finished Statistics.finish();
   *  `momentaryLufs` (optional) from a finished Loudness.finish(), for perceptual mode. */
  createSilence(
    audioBufferHandle: number,
    rmsSeries: Float32Array,
    rmsSeriesChannelCount: number,
    allZeroSeries: Uint8Array,
    momentaryLufs?: Float32Array,
  ): Silence | null {
    return Silence.create(this.module, audioBufferHandle, rmsSeries, rmsSeriesChannelCount, allZeroSeries, momentaryLufs);
  }

  /** `fileBytes` must be the complete file, not a probe slice — see Metadata.create()'s comment. */
  createMetadata(fileBytes: Uint8Array): Metadata | null {
    return Metadata.create(this.module, fileBytes);
  }

  dispose(): void {
    // The module itself has no explicit teardown in a single-instance-per-page model; this exists
    // for symmetry and for the worker-pool case (M20) where a module instance's lifetime is scoped.
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}
