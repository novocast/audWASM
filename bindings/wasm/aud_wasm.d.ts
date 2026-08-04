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

export interface AudModule {
  HEAPU8: Uint8Array;
  HEAPF32: Float32Array;
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
}

export interface CreateAudModuleOptions {
  wasmUrl?: string;
}

export default function createAudModule(opts?: CreateAudModuleOptions): Promise<AudModule>;
