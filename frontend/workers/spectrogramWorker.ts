// The spectrogram tile-generation worker (M07). First genuinely-wired worker in the app —
// decodeWorker.ts exists but nothing constructs it yet; this one is actually spun up by main.ts.
//
// Decision (M07 "worker/PCM ownership") — this worker gets its own copy of the decoded PCM and its
// own WASM module instance, rather than sharing the main thread's AudioBuffer. No SharedArrayBuffer
// is available (no COOP/COEP headers configured — see vite.config.ts), and cross-thread engine
// ownership is explicitly M20's problem, not M07's. The main thread posts a copy of the decoded
// per-channel PCM once (`loadPcm`), this worker builds its own AudioBuffer (via PcmBuffer) and its
// own Spectrogram/TileCache from it, and generates tiles entirely off the main thread — a 256-frame
// tile at fftSize 4096 is 5-10ms, easily enough to drop frames if done inline while scrolling.
//
// Cancellation: `requestTiles` enqueues, `setPriorities` reorders the pending queue (sent every
// frame by the main thread's tile manager — cheap, it's just an array of keys), `cancel` removes
// keys outright. The generation loop yields between tiles (a tile isn't preemptible mid-generation,
// but the queue is re-checked after every one), so a fast-scroll's cancellations actually take
// effect rather than the worker grinding through a stale backlog.

import { AudioEngine, type Spectrogram, type PcmBuffer } from '../../bindings/wasm/engine.ts';
import type {
  FftSpectrumScaling,
  FftWindowType,
  SpectrogramDecimation,
  SpectrogramFreqAxis,
} from '../../bindings/wasm/aud_wasm.d.ts';

export interface TileKeyMsg {
  level: number;
  tileX: number;
  channel: number;
}

interface LoadPcmMessage {
  type: 'loadPcm';
  channels: Float32Array[];
  sampleRate: number;
}

interface SetConfigMessage {
  type: 'setConfig';
  fftSize: number;
  window: FftWindowType;
  scaling: FftSpectrumScaling;
  freqAxis: SpectrogramFreqAxis;
  decimation: SpectrogramDecimation;
  minHz: number;
  floorDb: number;
  ceilDb: number;
}

interface RequestTilesMessage {
  type: 'requestTiles';
  keys: TileKeyMsg[];
}

interface SetPrioritiesMessage {
  type: 'setPriorities';
  keys: TileKeyMsg[];
}

interface CancelMessage {
  type: 'cancel';
  keys: TileKeyMsg[];
}

interface QueryPointMessage {
  type: 'queryPoint';
  requestId: number;
  channel: number;
  timeSeconds: number;
  targetHz: number;
}

type InboundMessage =
  | LoadPcmMessage
  | SetConfigMessage
  | RequestTilesMessage
  | SetPrioritiesMessage
  | CancelMessage
  | QueryPointMessage;

interface ReadyMessage {
  type: 'ready';
  channelCount: number;
  sampleRate: number;
}

interface ConfigChangedMessage {
  type: 'configChanged';
  configHash: number;
}

interface TileMessage {
  type: 'tile';
  key: TileKeyMsg;
  configHash: number;
  bytes: Uint8Array;
  floorDb: number;
  ceilDb: number;
  fftSize: number;
}

interface TileErrorMessage {
  type: 'tileError';
  key: TileKeyMsg;
  message: string;
}

interface OverviewMessage {
  type: 'overview';
  channel: number;
  bytes: Uint8Array;
  width: number;
  height: number;
  floorDb: number;
  ceilDb: number;
  fftSize: number;
}

interface PointResultMessage {
  type: 'pointResult';
  requestId: number;
  frequencyHz: number;
  magnitudeDb: number;
}

interface PointErrorMessage {
  type: 'pointError';
  requestId: number;
  message: string;
}

interface ErrorMessage {
  type: 'error';
  message: string;
}

type OutboundMessage =
  | ReadyMessage
  | ConfigChangedMessage
  | TileMessage
  | TileErrorMessage
  | OverviewMessage
  | PointResultMessage
  | PointErrorMessage
  | ErrorMessage;

function post(message: OutboundMessage, transfer?: Transferable[]): void {
  if (transfer && transfer.length > 0) {
    (postMessage as (m: unknown, t: Transferable[]) => void)(message, transfer);
  } else {
    postMessage(message);
  }
}

function keyOf(key: TileKeyMsg): string {
  return `${key.level}:${key.tileX}:${key.channel}`;
}

let enginePromise: Promise<AudioEngine> | null = null;
function getEngine(): Promise<AudioEngine> {
  enginePromise ??= AudioEngine.create();
  return enginePromise;
}

let pcmBuffer: PcmBuffer | null = null;
let spectrogram: Spectrogram | null = null;
let channelCount = 0;
let currentFftSize = 4096; // stamped into every tile/overview message — see renderer.ts's SpectrogramTileBytes.fftSize

// Pending-tile queue: an array (priority order, front = next to generate) plus a parallel Set for
// O(1) "is this still wanted" checks during cancel/dedup.
let pendingQueue: TileKeyMsg[] = [];
const pendingSet = new Set<string>();
let draining = false;

async function loadPcm(message: LoadPcmMessage): Promise<void> {
  const engine = await getEngine();
  channelCount = message.channels.length;

  pcmBuffer?.dispose();
  pcmBuffer = engine.createPcmBuffer(message.sampleRate, channelCount);
  if (!pcmBuffer) {
    post({ type: 'error', message: 'PcmBuffer.create failed' });
    return;
  }
  pcmBuffer.appendPlanar(message.channels);

  spectrogram?.dispose();
  spectrogram = engine.createSpectrogram(pcmBuffer.audioBufferHandle);
  if (!spectrogram) {
    post({ type: 'error', message: 'Spectrogram.create failed' });
    return;
  }

  pendingQueue = [];
  pendingSet.clear();

  post({ type: 'ready', channelCount, sampleRate: message.sampleRate });
}

function setConfig(message: SetConfigMessage): void {
  if (!spectrogram) {
    post({ type: 'error', message: 'setConfig received before loadPcm' });
    return;
  }
  const configHash = spectrogram.setConfig(
    message.fftSize,
    message.window,
    message.scaling,
    message.freqAxis,
    message.decimation,
    message.minHz,
    message.floorDb,
    message.ceilDb,
  );
  currentFftSize = message.fftSize;

  // Old-hash in-flight requests are simply left in the queue and will regenerate against whatever
  // the live config now is by the time they're popped (requestTile() always uses the cache's
  // *current* config) — harmless, since the client-side tile manager keys its atlas by
  // (key, configHash) and will just treat that result as "the new config's tile for this slot"
  // (M07: "old tiles shown until new ones arrive", not "old requests must complete against the old
  // config").
  post({ type: 'configChanged', configHash });

  // Emit the (cheap, single-pass) overview for every channel eagerly under the new config — M07's
  // "resident overview" decision, refreshed whenever the generation config changes.
  for (let ch = 0; ch < channelCount; ch++) {
    try {
      const overview = spectrogram.overview(ch);
      post(
        {
          type: 'overview',
          channel: ch,
          bytes: overview.bytes,
          width: overview.width,
          height: overview.height,
          floorDb: overview.floorDb,
          ceilDb: overview.ceilDb,
          fftSize: currentFftSize,
        },
        [overview.bytes.buffer],
      );
    } catch (err) {
      post({ type: 'error', message: `overview(${ch}) failed: ${err instanceof Error ? err.message : String(err)}` });
    }
  }

  drainQueue();
}

function requestTiles(keys: TileKeyMsg[]): void {
  for (const key of keys) {
    const id = keyOf(key);
    if (!pendingSet.has(id)) {
      pendingSet.add(id);
      pendingQueue.push(key);
    }
  }
  drainQueue();
}

function setPriorities(keys: TileKeyMsg[]): void {
  // Reorders the pending queue to match `keys`' order (front = highest priority = closest to
  // viewport centre, per the main thread's tile manager); anything pending but not mentioned keeps
  // its relative order at the back.
  const wanted = new Map(keys.map((k, i) => [keyOf(k), i]));
  pendingQueue.sort((a, b) => {
    const ai = wanted.get(keyOf(a));
    const bi = wanted.get(keyOf(b));
    if (ai !== undefined && bi !== undefined) return ai - bi;
    if (ai !== undefined) return -1;
    if (bi !== undefined) return 1;
    return 0;
  });
}

function cancel(keys: TileKeyMsg[]): void {
  for (const key of keys) {
    pendingSet.delete(keyOf(key));
  }
  pendingQueue = pendingQueue.filter((k) => pendingSet.has(keyOf(k)));
}

function drainQueue(): void {
  if (draining) return;
  draining = true;
  void (async () => {
    while (pendingQueue.length > 0) {
      const key = pendingQueue.shift()!;
      const id = keyOf(key);
      if (!pendingSet.has(id)) continue; // cancelled while queued
      pendingSet.delete(id);

      if (!spectrogram) break;
      try {
        const tile = spectrogram.requestTile(key.level, key.tileX, key.channel);
        post(
          {
            type: 'tile',
            key,
            configHash: spectrogram.currentConfigHash,
            bytes: tile.bytes,
            floorDb: tile.floorDb,
            ceilDb: tile.ceilDb,
            fftSize: currentFftSize,
          },
          [tile.bytes.buffer],
        );
      } catch (err) {
        post({ type: 'tileError', key, message: err instanceof Error ? err.message : String(err) });
      }

      // Yield back to the event loop between tiles so a `cancel`/`setPriorities` message that
      // arrived while this one was generating actually gets processed before the next pop —
      // otherwise the worker would grind through a stale backlog during a fast scroll.
      await Promise.resolve();
    }
    draining = false;
  })();
}

function queryPoint(message: QueryPointMessage): void {
  if (!spectrogram) {
    post({ type: 'pointError', requestId: message.requestId, message: 'queryPoint received before loadPcm' });
    return;
  }
  try {
    const result = spectrogram.queryPoint(message.channel, message.timeSeconds, message.targetHz);
    post({ type: 'pointResult', requestId: message.requestId, frequencyHz: result.frequencyHz, magnitudeDb: result.magnitudeDb });
  } catch (err) {
    post({ type: 'pointError', requestId: message.requestId, message: err instanceof Error ? err.message : String(err) });
  }
}

self.onmessage = (event: MessageEvent<InboundMessage>) => {
  const message = event.data;
  switch (message.type) {
    case 'loadPcm':
      loadPcm(message).catch((err: unknown) => {
        post({ type: 'error', message: err instanceof Error ? err.message : String(err) });
      });
      break;
    case 'setConfig':
      setConfig(message);
      break;
    case 'requestTiles':
      requestTiles(message.keys);
      break;
    case 'setPriorities':
      setPriorities(message.keys);
      break;
    case 'cancel':
      cancel(message.keys);
      break;
    case 'queryPoint':
      queryPoint(message);
      break;
  }
};
