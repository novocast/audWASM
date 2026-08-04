// Main-thread playback API + observable state. Wires together:
//   - the WASM Transport (bindings/wasm/engine.ts) — state machine, resampling, gain
//   - a PcmSink (sabSink.ts or postMessageSink.ts, chosen via capabilities.ts)
//   - the AudioWorkletNode running audProcessor.ts
//   - positionClock.ts for a smooth 60fps position readout
//
// This is the seam the M03 task list calls "transportClient.ts — main-thread API + observable
// state". UI code (and M19's debugger, later) should only ever talk to this class, never touch the
// WASM Transport or the sink directly.

import type { AudioEngine, DecodeSession, Transport as WasmTransport } from '../../../bindings/wasm/engine.ts';
import type { TransportStatusName } from '../../../bindings/wasm/aud_wasm.d.ts';
import { detectCapabilities } from './capabilities.ts';
import type { PcmSinkConfig, PcmSinkWriter } from './pcmSink.ts';
import { allocateSabRing, SabRingWriter } from './sabSink.ts';
import { PostMessageSinkWriter } from './postMessageSink.ts';
import { PositionClock } from './positionClock.ts';

export type { TransportStatusName };

export interface TransportObservableState {
  status: TransportStatusName;
  positionSeconds: number;
  durationSeconds: number | null; // null while unknown (kNoFrame)
  loopEnabled: boolean;
  loopStartSeconds: number;
  loopEndSeconds: number;
  gain: number;
  dropoutCount: number;
  usingSharedArrayBuffer: boolean;
}

export interface TransportClientOptions {
  audioContext: AudioContext;
  engine: AudioEngine;
  decodeSession: DecodeSession;
  sourceSampleRate: number;
  channelCount: number;
  /** Worklet module URL, e.g. `new URL('./worklet/audProcessor.ts', import.meta.url)`. */
  workletModuleUrl: string | URL;
  /** Defaults to ~500ms of output-rate audio (M03 risk table: "prebuffer >= 250ms"). */
  ringCapacityFrames?: number;
  /** How often (ms) to pump the producer while playing. */
  pumpIntervalMs?: number;
}

const kPumpFrames = 4096;

export class TransportClient {
  private readonly listeners = new Set<(state: TransportObservableState) => void>();
  private pumpTimer: ReturnType<typeof setInterval> | null = null;
  private dropoutCount = 0;
  private disposed = false;

  private constructor(
    private readonly opts: TransportClientOptions,
    private readonly wasmTransport: WasmTransport,
    private readonly sinkWriter: PcmSinkWriter,
    private readonly workletNode: AudioWorkletNode,
    private readonly positionClock: PositionClock,
  ) {
    // addEventListener, not `port.onmessage =`: in postMessage-sink mode, PostMessageSinkWriter
    // already listens on this same port for 'pcmConsumed' credit updates, and the single-slot
    // onmessage property would clobber whichever of the two was assigned second.
    this.workletNode.port.addEventListener('message', (event: MessageEvent) => {
      if (event.data?.type === 'dropout') {
        this.dropoutCount += 1;
      }
    });
    this.workletNode.port.start();
  }

  static async create(opts: TransportClientOptions): Promise<TransportClient> {
    const caps = detectCapabilities();
    const ringCapacityFrames = opts.ringCapacityFrames ?? Math.ceil(opts.audioContext.sampleRate * 0.5);

    await opts.audioContext.audioWorklet.addModule(opts.workletModuleUrl);

    let sinkConfig: PcmSinkConfig;
    let sinkWriter: PcmSinkWriter;
    if (caps.sharedArrayBuffer) {
      const { buffer, capacity } = allocateSabRing(opts.channelCount, ringCapacityFrames);
      sinkWriter = new SabRingWriter(buffer, opts.channelCount, capacity);
      sinkConfig = { mode: 'sab', buffer, channelCount: opts.channelCount, capacityFrames: capacity };
    } else {
      sinkConfig = { mode: 'postMessage', channelCount: opts.channelCount };
      // constructed after the node exists, since the writer needs the node's port — see below.
      sinkWriter = null as unknown as PcmSinkWriter;
    }

    const workletNode = new AudioWorkletNode(opts.audioContext, 'aud-processor', {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [opts.channelCount],
      processorOptions: sinkConfig,
    });
    workletNode.connect(opts.audioContext.destination);

    if (sinkConfig.mode === 'postMessage') {
      sinkWriter = new PostMessageSinkWriter(workletNode.port, opts.channelCount, ringCapacityFrames);
    }

    const wasmTransport = opts.engine.createTransport(
      opts.sourceSampleRate,
      opts.audioContext.sampleRate,
      opts.channelCount,
      ringCapacityFrames,
    );
    if (!wasmTransport) {
      throw new Error('failed to create playback Transport (invalid sample rate/channel count)');
    }
    wasmTransport.attachSource(opts.decodeSession.audioBufferHandle);

    const getLatencySeconds = (): number => {
      const ctx = opts.audioContext as AudioContext & { outputLatency?: number };
      return ctx.outputLatency ?? opts.audioContext.baseLatency ?? 0;
    };
    const positionClock = new PositionClock({
      sourceSampleRate: opts.sourceSampleRate,
      getAuthoritativeSourceFrame: () => wasmTransport.state.positionFrames,
      getLatencySeconds,
    });

    const client = new TransportClient(opts, wasmTransport, sinkWriter, workletNode, positionClock);
    client.startPumpLoop();
    return client;
  }

  /** Signals decode has finished (M02's DecodeSession.finish() already ran). Until this is
   *  called, running off the end of what's decoded so far parks the transport in `loading` rather
   *  than `ended`. */
  setSourceComplete(complete: boolean): void {
    this.wasmTransport.setSourceComplete(complete);
  }

  load(durationFrames: number | null): void {
    this.wasmTransport.load();
    this.wasmTransport.ready(durationFrames ?? -1);
  }

  /** Must be called from within a user gesture the first time (autoplay policy) — this is what
   *  resumes the AudioContext. */
  async play(): Promise<void> {
    if (this.opts.audioContext.state !== 'running') {
      await this.opts.audioContext.resume();
    }
    this.wasmTransport.play();
    this.notify();
  }

  pause(): void {
    this.wasmTransport.pause();
    this.notify();
  }

  seekToSeconds(seconds: number): void {
    const targetFrame = Math.round(seconds * this.opts.sourceSampleRate);
    this.wasmTransport.seekTo(targetFrame);
    this.sinkWriter.reset();
    this.positionClock.reset(targetFrame);
    this.notify();
  }

  setLoop(enabled: boolean, startSeconds?: number, endSeconds?: number): void {
    if (startSeconds !== undefined && endSeconds !== undefined) {
      this.wasmTransport.setLoopRange(
        Math.round(startSeconds * this.opts.sourceSampleRate),
        Math.round(endSeconds * this.opts.sourceSampleRate),
      );
    }
    this.wasmTransport.setLoopEnabled(enabled);
    this.notify();
  }

  setGain(gain: number): void {
    this.wasmTransport.setGain(gain);
    this.notify();
  }

  /** Call once per animation frame from the UI's rAF loop for a smooth, latency-corrected cursor. */
  tick(nowMs: number): TransportObservableState {
    this.positionClock.tick(nowMs);
    return this.currentState();
  }

  subscribe(listener: (state: TransportObservableState) => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    if (this.pumpTimer !== null) clearInterval(this.pumpTimer);
    this.workletNode.disconnect();
    this.wasmTransport.dispose();
  }

  private currentState(): TransportObservableState {
    const raw = this.wasmTransport.state;
    const rate = this.opts.sourceSampleRate;
    return {
      status: raw.status,
      positionSeconds: this.positionClock.positionSeconds,
      durationSeconds: raw.durationFrames < 0 ? null : raw.durationFrames / rate,
      loopEnabled: raw.loopEnabled,
      loopStartSeconds: raw.loopBegin / rate,
      loopEndSeconds: raw.loopEnd / rate,
      gain: raw.gain,
      dropoutCount: this.dropoutCount,
      usingSharedArrayBuffer: this.sinkWriter instanceof SabRingWriter,
    };
  }

  private notify(): void {
    const state = this.currentState();
    for (const listener of this.listeners) listener(state);
  }

  private startPumpLoop(): void {
    const pump = (): void => {
      const status = this.wasmTransport.state.status;
      const isActive = status === 'playing' || status === 'seeking';
      if (isActive) {
        this.wasmTransport.pump(kPumpFrames);
        const headroom = this.sinkWriter.framesAvailableToWrite();
        if (headroom > 0) {
          const rendered = this.wasmTransport.render(Math.min(headroom, kPumpFrames), this.opts.channelCount);
          this.sinkWriter.write(rendered, rendered[0]?.length ?? 0);
        }
      }
      this.notify();
    };
    this.pumpTimer = setInterval(pump, this.opts.pumpIntervalMs ?? 20);
  }
}
