// Idiomatic TS wrapper over the raw Embind surface. `create()` is the only async entry point —
// everything after instantiation is synchronous from JS's point of view (M01).

import createAudModule from './aud_wasm.js';
import type {
  AudModule,
  CreateAudModuleOptions,
  DecodeDiagnostic,
  EngineBuildInfo,
  RawDecodeSessionHandle,
  StreamInfo,
} from './engine.d.ts';
import { copyIntoHeap, noteGrowthBoundary } from './heap_view.ts';

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

  dispose(): void {
    // The module itself has no explicit teardown in a single-instance-per-page model; this exists
    // for symmetry and for the worker-pool case (M20) where a module instance's lifetime is scoped.
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}
