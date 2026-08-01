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
  delete(): void; // Embind-generated; required to release the underlying C++ object
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
}

export interface CreateAudModuleOptions {
  wasmUrl?: string;
}

export default function createAudModule(opts?: CreateAudModuleOptions): Promise<AudModule>;
