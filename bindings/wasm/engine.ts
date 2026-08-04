// Idiomatic TS wrapper over the raw Embind surface. `create()` is the only async entry point —
// everything after instantiation is synchronous from JS's point of view (M01).

import createAudModule from './aud_wasm.js';
import type {
  AudModule,
  CreateAudModuleOptions,
  DecodeDiagnostic,
  EngineBuildInfo,
  OperationResult,
  RawDecodeSessionHandle,
  RawTransportHandle,
  RawTransportState,
  RawWaveformHandle,
  StreamInfo,
  WaveformChannelSelector,
} from './aud_wasm.d.ts';
import { copyIntoHeap, float32View, noteGrowthBoundary } from './heap_view.ts';
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

  dispose(): void {
    // The module itself has no explicit teardown in a single-instance-per-page model; this exists
    // for symmetry and for the worker-pool case (M20) where a module instance's lifetime is scoped.
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}
