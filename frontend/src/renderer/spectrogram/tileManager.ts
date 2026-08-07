// Client-side counterpart to the spectrogram worker (M07 "Tile pipeline"): computes which tiles
// the current viewport (+ 1-tile margin) needs, posts requests/cancellations/priorities to the
// worker, and stores whatever bytes have arrived so far. Implements `SpectrogramSource`
// (renderer.ts) — the WebGL program pulls plain tile bytes from here, never a WASM/worker type.
//
// Decision — a stale-config tile is never evicted just because the config changed. The map here is
// keyed by (level, tileX, channel) only, not configHash: a fresh tile for the same slot simply
// overwrites whatever was there. That is what gives M07's "old tiles shown until new ones arrive, no
// flash of empty" for free, without special-casing anything — the alternative (clearing on
// configChanged) would blank the whole spectrogram the instant the user touches a control.

import type { ViewState } from '../viewState.ts';
import type { SpectrogramSource, SpectrogramTileBytes, SpectrogramTileRef } from '../renderer.ts';

export const kTileWidth = 256;
export const kTileHeight = 256;

/** The three fixed hop-derived time-resolution levels (M07 "Time resolution levels") plus the
 *  fold-factor rule for coarser ones — mirrors engine/spectrogram/tile_generator.hpp exactly so the
 *  client's idea of "which level covers this time span" matches what the engine actually generates. */
export function hopForLevel(fftSize: number, level: number): number {
  if (level === 0) return fftSize / 4;
  if (level === 1) return fftSize / 2;
  return fftSize;
}

export function foldFactorForLevel(level: number): number {
  return level <= 2 ? 1 : Math.pow(2, level - 2);
}

/** Samples per output column at `level` — the metric level selection matches against
 *  `framesPerPixel`, and the tile's full time span (`kTileWidth` columns' worth). */
export function samplesPerColumn(fftSize: number, level: number): number {
  return hopForLevel(fftSize, level) * foldFactorForLevel(level);
}

function tileSpanSamples(fftSize: number, level: number): number {
  return kTileWidth * samplesPerColumn(fftSize, level);
}

/** Picks the level whose column spacing is closest to the current zoom (framesPerPixel) — same
 *  "closest, not necessarily coarser" spirit as the waveform pyramid's level selection, just over a
 *  small fixed set of levels rather than a full mipmap. */
export function chooseLevel(fftSize: number, framesPerPixel: number, maxLevel = 8): number {
  let best = 0;
  let bestDiff = Infinity;
  for (let level = 0; level <= maxLevel; level++) {
    const diff = Math.abs(samplesPerColumn(fftSize, level) - framesPerPixel);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = level;
    }
  }
  return best;
}

// M07 "fftSize also adapts with zoom ... in coarse steps (2048 -> 8192 -> 16384) with hysteresis".
export const kFftSizeSteps = [2048, 8192, 16384] as const;
// Boundaries between adjacent steps, in framesPerPixel units; picked so a 256-column tile at each
// step roughly spans a comparable number of screen pixels at the boundary.
const kFftSizeBoundaries = [80, 800] as const;
const kHysteresis = 0.25; // must cross 25% past a boundary before switching, in either direction

/** Pure function, unit-tested against a scripted continuous zoom (M07 acceptance criteria): steps
 *  only across kFftSizeSteps, never continuously, and requires crossing well past a boundary to
 *  switch — so a small back-and-forth wheel nudge near a boundary doesn't thrash fftSize (which
 *  would mean regenerating every visible tile on every nudge). */
export function chooseFftSize(framesPerPixel: number, currentFftSize: number): number {
  const steps = kFftSizeSteps;
  const startIndex = steps.indexOf(currentFftSize as (typeof steps)[number]);
  let index = startIndex === -1 ? 1 : startIndex;

  while (index < steps.length - 1 && framesPerPixel > kFftSizeBoundaries[index]! * (1 + kHysteresis)) {
    index++;
  }
  while (index > 0 && framesPerPixel < kFftSizeBoundaries[index - 1]! * (1 - kHysteresis)) {
    index--;
  }
  return steps[index]!;
}

function keyOf(ref: SpectrogramTileRef): string {
  return `${ref.level}:${ref.tileX}:${ref.channel}`;
}

function refOf(key: string): SpectrogramTileRef {
  const [level, tileX, channel] = key.split(':').map(Number);
  return { level: level!, tileX: tileX!, channel: channel! };
}

/** Computes the priority-ordered (closest to viewport centre first) set of tiles the current view
 *  + a 1-tile margin on each side needs, for every channel. Exported standalone for unit testing. */
export function computeVisibleTiles(view: ViewState, fftSize: number, channelCount: number): SpectrogramTileRef[] {
  if (channelCount <= 0 || view.widthCss <= 0) return [];

  const level = chooseLevel(fftSize, view.framesPerPixel);
  const span = tileSpanSamples(fftSize, level);
  if (span <= 0) return [];

  const widthPx = Math.max(1, Math.round(view.widthCss * view.devicePixelRatio));
  const startFrame = view.startFrame;
  const endFrame = startFrame + view.framesPerPixel * widthPx;

  const tileXLo = Math.max(0, Math.floor(startFrame / span) - 1);
  const tileXHi = Math.floor(endFrame / span) + 1;
  const centreFrame = (startFrame + endFrame) / 2;

  const refs: SpectrogramTileRef[] = [];
  for (let tileX = tileXLo; tileX <= tileXHi; tileX++) {
    for (let channel = 0; channel < channelCount; channel++) {
      refs.push({ level, tileX, channel });
    }
  }

  refs.sort((a, b) => {
    const centreA = a.tileX * span + span / 2;
    const centreB = b.tileX * span + span / 2;
    return Math.abs(centreA - centreFrame) - Math.abs(centreB - centreFrame);
  });

  return refs;
}

interface WorkerTileMessage {
  type: 'ready' | 'configChanged' | 'tile' | 'tileError' | 'overview' | 'pointResult' | 'pointError' | 'error';
  [key: string]: unknown;
}

interface PendingPointQuery {
  resolve: (value: { frequencyHz: number; magnitudeDb: number }) => void;
  reject: (reason: Error) => void;
}

export class SpectrogramTileManager implements SpectrogramSource {
  configHash = 0;

  private channelCount = 0;
  private sampleRateValue = 44100;
  private tiles = new Map<string, SpectrogramTileBytes>();
  private overviews = new Map<number, SpectrogramTileBytes>();
  private outstanding = new Set<string>();
  private currentFftSize: number = kFftSizeSteps[1];
  private view: ViewState | null = null;
  private nextRequestId = 1;
  private pendingPointQueries = new Map<number, PendingPointQuery>();

  constructor(private readonly worker: Worker, private readonly onUpdate: () => void) {
    worker.onmessage = (event: MessageEvent<WorkerTileMessage>) => this.handleMessage(event.data);
  }

  get isReady(): boolean {
    return this.channelCount > 0;
  }

  get sampleRate(): number {
    return this.sampleRateValue;
  }

  /** Resident tile count (across every config generation ever seen, not just the current one —
   *  see the class comment on why stale-config tiles aren't evicted eagerly). Diagnostic/UI use
   *  only, not a substitute for the engine's own byte-budget accounting. */
  get residentTileCount(): number {
    return this.tiles.size;
  }

  /** Copies `channels` to the worker once (M07 "worker/PCM ownership" — a copy, not a transfer, the
   *  main thread still needs its own for waveform queries). Clears every resident tile/overview
   *  first — unlike a config change (where stale-config tiles are deliberately kept until replaced,
   *  see the class comment), a *new track* makes every previous tile's (level,tileX,channel)
   *  meaningless, so there is nothing worth keeping around. */
  loadPcm(channels: readonly Float32Array[], sampleRate: number): void {
    this.tiles.clear();
    this.overviews.clear();
    this.outstanding.clear();
    this.worker.postMessage({ type: 'loadPcm', channels: channels.slice(), sampleRate });
  }

  setConfig(
    fftSize: number,
    window: number,
    scaling: number,
    freqAxis: number,
    decimation: number,
    minHz: number,
    floorDb: number,
    ceilDb: number,
  ): void {
    this.currentFftSize = fftSize;
    this.worker.postMessage({ type: 'setConfig', fftSize, window, scaling, freqAxis, decimation, minHz, floorDb, ceilDb });
  }

  /** Call once per frame (from the render loop, before render()) with the latest ViewState — this
   *  is what recomputes the wanted tile set and drives requests/cancellations/priorities. Does
   *  *not* itself decide when fftSize/window/axis/decimation should change; the caller compares
   *  against its own last-applied config and calls setConfig() when something actually changed. */
  update(view: ViewState): void {
    this.view = view;
    if (!this.isReady) return;

    const fftSize = view.spectrogram.fftSizeOverride ?? chooseFftSize(view.framesPerPixel, this.currentFftSize);
    const refs = computeVisibleTiles(view, fftSize, this.channelCount);
    const wanted = new Set(refs.map(keyOf));

    const toCancel = [...this.outstanding].filter((k) => !wanted.has(k));
    if (toCancel.length > 0) {
      for (const k of toCancel) this.outstanding.delete(k);
      this.worker.postMessage({ type: 'cancel', keys: toCancel.map(refOf) });
    }

    const toRequest = refs.filter((r) => !this.tiles.has(keyOf(r)) && !this.outstanding.has(keyOf(r)));
    if (toRequest.length > 0) {
      for (const r of toRequest) this.outstanding.add(keyOf(r));
      this.worker.postMessage({ type: 'requestTiles', keys: toRequest });
    }

    if (refs.length > 0) {
      this.worker.postMessage({ type: 'setPriorities', keys: refs });
    }
  }

  queryPoint(channel: number, timeSeconds: number, targetHz: number): Promise<{ frequencyHz: number; magnitudeDb: number }> {
    const requestId = this.nextRequestId++;
    return new Promise((resolve, reject) => {
      this.pendingPointQueries.set(requestId, { resolve, reject });
      this.worker.postMessage({ type: 'queryPoint', requestId, channel, timeSeconds, targetHz });
    });
  }

  // --- SpectrogramSource -----------------------------------------------------------------------

  visibleTiles(): readonly SpectrogramTileRef[] {
    if (!this.view || !this.isReady) return [];
    const fftSize = this.view.spectrogram.fftSizeOverride ?? this.currentFftSize;
    return computeVisibleTiles(this.view, fftSize, this.channelCount);
  }

  tileBytes(ref: SpectrogramTileRef): SpectrogramTileBytes | null {
    return this.tiles.get(keyOf(ref)) ?? null;
  }

  overviewBytes(channel: number): SpectrogramTileBytes | null {
    return this.overviews.get(channel) ?? null;
  }

  // ----------------------------------------------------------------------------------------------

  private handleMessage(message: WorkerTileMessage): void {
    switch (message.type) {
      case 'ready':
        this.channelCount = message.channelCount as number;
        this.sampleRateValue = message.sampleRate as number;
        this.onUpdate();
        break;
      case 'configChanged':
        // Deliberately does NOT clear `this.tiles` — see the file header decision.
        this.configHash = message.configHash as number;
        break;
      case 'tile': {
        const ref = message.key as SpectrogramTileRef;
        const key = keyOf(ref);
        this.outstanding.delete(key);
        this.tiles.set(key, {
          bytes: message.bytes as Uint8Array,
          width: kTileWidth,
          height: kTileHeight,
          floorDb: message.floorDb as number,
          ceilDb: message.ceilDb as number,
          fftSize: message.fftSize as number,
        });
        this.onUpdate();
        break;
      }
      case 'tileError': {
        const ref = message.key as SpectrogramTileRef;
        this.outstanding.delete(keyOf(ref));
        break;
      }
      case 'overview': {
        this.overviews.set(message.channel as number, {
          bytes: message.bytes as Uint8Array,
          width: message.width as number,
          height: message.height as number,
          floorDb: message.floorDb as number,
          ceilDb: message.ceilDb as number,
          fftSize: message.fftSize as number,
        });
        this.onUpdate();
        break;
      }
      case 'pointResult': {
        const pending = this.pendingPointQueries.get(message.requestId as number);
        pending?.resolve({ frequencyHz: message.frequencyHz as number, magnitudeDb: message.magnitudeDb as number });
        this.pendingPointQueries.delete(message.requestId as number);
        break;
      }
      case 'pointError': {
        const pending = this.pendingPointQueries.get(message.requestId as number);
        pending?.reject(new Error(message.message as string));
        this.pendingPointQueries.delete(message.requestId as number);
        break;
      }
      case 'error':
        console.error(`[spectrogram worker] ${String(message.message)}`);
        break;
    }
  }

  dispose(): void {
    this.worker.terminate();
  }
}
