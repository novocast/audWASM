// The single shared, authoritative view model (M17 "View model"), owned outside the renderer, plus
// a reducer that owns all clamping/limits. Nothing else may mutate a ViewState field directly —
// that's what keeps `startFrame`/`framesPerPixel` internally consistent (e.g. never zoomed past a
// single frame across the whole width, never panned past the track's ends).

import { clamp, zoomAnchoredStartFrame, type FreqAxis } from './coords.ts';

export type ChannelLayout = 'split' | 'overlaid' | 'monoSum' | 'midSide';
export type AmplitudeScaleType = 'linear' | 'db' | 'root';
export type FollowMode = 'off' | 'page' | 'scroll' | 'centre';

export interface AmplitudeScale {
  type: AmplitudeScaleType;
  /** nth-root exponent when type === 'root'; ignored otherwise. */
  param: number;
}

// M07's colour maps (data, not code — see frontend/src/renderer/spectrogram/colormaps.ts).
export type SpectrogramColorMap = 'viridis' | 'magma' | 'inferno' | 'greyscale' | 'classic';
export type SpectrogramDecimationMode = 'max' | 'mean';

// Decision — this is deliberately split into shader-only fields (applied as fragment-shader
// uniforms against already-generated tiles, so changing them is instant per M07's acceptance
// criteria) and tile-config fields (fed to the spectrogram worker's setConfig(), which regenerates
// tiles). `displayFloorDb`/`gainDb`/`gamma`/`colorMap` are the former; `freqAxis`/`fftSizeOverride`/
// `decimation` are the latter. `displayFloorDb` is a *display* dB floor for the colour ramp,
// independent of the tile's own quantisation floor/ceiling baked in at generation time (M07 "Tiles"
// decision) — changing it never touches a tile's bytes.
export interface SpectrogramSettings {
  colorMap: SpectrogramColorMap;
  displayFloorDb: number;
  gainDb: number;
  gamma: number;
  freqAxis: FreqAxis;
  /** null = adaptive (M07 "fftSize also adapts with zoom"); a concrete value overrides adaptation. */
  fftSizeOverride: number | null;
  decimation: SpectrogramDecimationMode;
}

export interface ViewState {
  /** Left edge, in source frames. Fractional — see M17 "startFrame is fractional". */
  startFrame: number;
  /** Zoom primitive: source frames per CSS pixel. Fractional; the *only* zoom representation. */
  framesPerPixel: number;
  devicePixelRatio: number;
  widthCss: number;
  heightCss: number;
  channelLayout: ChannelLayout;
  amplitudeScale: AmplitudeScale;
  verticalZoom: number;
  followPlayhead: FollowMode;
  /** True while a manual pan/zoom has temporarily suspended `followPlayhead` (M17 "Follow modes").
   *  Cleared by the re-centre affordance or by explicitly re-enabling follow. */
  followSuspended: boolean;
  spectrogram: SpectrogramSettings;
}

export interface ViewLimits {
  totalFrames: number;
  /** Smallest allowed framesPerPixel — the "single sample across the full width" extreme. */
  minFramesPerPixel: number;
  /** Largest allowed framesPerPixel — derived from totalFrames unless overridden. */
  maxFramesPerPixel?: number;
  minVerticalZoom?: number;
  maxVerticalZoom?: number;
}

export const kDefaultMinFramesPerPixel = 1 / 64;

export function defaultViewState(overrides: Partial<ViewState> = {}): ViewState {
  return {
    startFrame: 0,
    framesPerPixel: 1,
    devicePixelRatio: 1,
    widthCss: 0,
    heightCss: 0,
    channelLayout: 'split',
    amplitudeScale: { type: 'linear', param: 1 },
    verticalZoom: 1,
    followPlayhead: 'off',
    followSuspended: false,
    spectrogram: {
      colorMap: 'viridis',
      displayFloorDb: -96,
      gainDb: 0,
      gamma: 1,
      freqAxis: 'log',
      fftSizeOverride: null,
      decimation: 'max',
    },
    ...overrides,
  };
}

export type ViewAction =
  | { type: 'resize'; widthCss: number; heightCss: number }
  | { type: 'setDpr'; devicePixelRatio: number }
  | { type: 'pan'; deltaFrames: number }
  | { type: 'panToStart'; startFrame: number }
  /** Programmatic move driven by follow-mode tracking (loop.ts), as opposed to a user gesture —
   *  must NOT suspend follow the way 'pan'/'panToStart' do, or the playhead would desync from the
   *  view on the very next frame after the mode it's supposed to be honouring moved it. */
  | { type: 'followMove'; startFrame: number }
  | { type: 'zoomAt'; anchorPixel: number; factor: number }
  | { type: 'setFramesPerPixel'; framesPerPixel: number; anchorPixel?: number }
  | { type: 'zoomToRange'; startFrame: number; endFrame: number }
  | { type: 'setVerticalZoom'; verticalZoom: number }
  | { type: 'setChannelLayout'; channelLayout: ChannelLayout }
  | { type: 'setAmplitudeScale'; amplitudeScale: AmplitudeScale }
  | { type: 'setFollowMode'; followPlayhead: FollowMode }
  | { type: 'suspendFollow' }
  | { type: 'resumeFollow' }
  | { type: 'setColorMap'; colorMap: SpectrogramColorMap }
  | { type: 'setSpectrogramRange'; displayFloorDb: number; gainDb: number; gamma: number }
  | { type: 'setFreqAxis'; freqAxis: FreqAxis }
  | { type: 'setFftSizeOverride'; fftSizeOverride: number | null }
  | { type: 'setDecimation'; decimation: SpectrogramDecimationMode };

function maxFramesPerPixelFor(limits: ViewLimits, widthCss: number): number {
  if (limits.maxFramesPerPixel !== undefined) return limits.maxFramesPerPixel;
  // "Zoomed all the way out" = the whole track fits in the current width; guard widthCss<=0
  // (not yet laid out) with a permissive fallback so it isn't spuriously clamped to 0.
  if (widthCss <= 0) return Math.max(limits.totalFrames, limits.minFramesPerPixel);
  return Math.max(limits.totalFrames / widthCss, limits.minFramesPerPixel);
}

function clampFramesPerPixel(framesPerPixel: number, limits: ViewLimits, widthCss: number): number {
  const max = maxFramesPerPixelFor(limits, widthCss);
  return clamp(framesPerPixel, limits.minFramesPerPixel, Math.max(max, limits.minFramesPerPixel));
}

function clampStartFrame(startFrame: number, framesPerPixel: number, limits: ViewLimits, widthCss: number): number {
  const visibleFrames = framesPerPixel * widthCss;
  // A view wider than the track is centred rather than pinned to 0, which reads better than a
  // wall of empty space on the right at extreme zoom-out.
  if (visibleFrames >= limits.totalFrames) {
    return (limits.totalFrames - visibleFrames) / 2;
  }
  return clamp(startFrame, 0, limits.totalFrames - visibleFrames);
}

/** Applies one action, then re-clamps every field the action could have pushed out of range.
 *  Clamping lives here, and only here — no other module recomputes these limits. */
export function viewReducer(state: ViewState, action: ViewAction, limits: ViewLimits): ViewState {
  switch (action.type) {
    case 'resize': {
      const widthCss = Math.max(0, action.widthCss);
      const heightCss = Math.max(0, action.heightCss);
      const framesPerPixel = clampFramesPerPixel(state.framesPerPixel, limits, widthCss);
      const startFrame = clampStartFrame(state.startFrame, framesPerPixel, limits, widthCss);
      return { ...state, widthCss, heightCss, framesPerPixel, startFrame };
    }
    case 'setDpr':
      return { ...state, devicePixelRatio: Math.max(action.devicePixelRatio, 0.1) };
    case 'pan': {
      const startFrame = clampStartFrame(
        state.startFrame + action.deltaFrames,
        state.framesPerPixel,
        limits,
        state.widthCss,
      );
      return applyFollowSuspendOnManualMove(state, startFrame);
    }
    case 'panToStart': {
      const startFrame = clampStartFrame(action.startFrame, state.framesPerPixel, limits, state.widthCss);
      return applyFollowSuspendOnManualMove(state, startFrame);
    }
    case 'followMove': {
      const startFrame = clampStartFrame(action.startFrame, state.framesPerPixel, limits, state.widthCss);
      return state.startFrame === startFrame ? state : { ...state, startFrame };
    }
    case 'zoomAt': {
      const framesPerPixel = clampFramesPerPixel(state.framesPerPixel * action.factor, limits, state.widthCss);
      const rawStart = zoomAnchoredStartFrame(action.anchorPixel, state, framesPerPixel);
      const startFrame = clampStartFrame(rawStart, framesPerPixel, limits, state.widthCss);
      return { ...applyFollowSuspendOnManualMove(state, startFrame), framesPerPixel };
    }
    case 'setFramesPerPixel': {
      const framesPerPixel = clampFramesPerPixel(action.framesPerPixel, limits, state.widthCss);
      const anchorPixel = action.anchorPixel ?? state.widthCss / 2;
      const rawStart = zoomAnchoredStartFrame(anchorPixel, state, framesPerPixel);
      const startFrame = clampStartFrame(rawStart, framesPerPixel, limits, state.widthCss);
      return { ...applyFollowSuspendOnManualMove(state, startFrame), framesPerPixel };
    }
    case 'zoomToRange': {
      const span = Math.max(action.endFrame - action.startFrame, limits.minFramesPerPixel);
      const framesPerPixel = clampFramesPerPixel(
        state.widthCss > 0 ? span / state.widthCss : span,
        limits,
        state.widthCss,
      );
      const startFrame = clampStartFrame(action.startFrame, framesPerPixel, limits, state.widthCss);
      return { ...applyFollowSuspendOnManualMove(state, startFrame), framesPerPixel };
    }
    case 'setVerticalZoom': {
      const min = limits.minVerticalZoom ?? 0.1;
      const max = limits.maxVerticalZoom ?? 50;
      return { ...state, verticalZoom: clamp(action.verticalZoom, min, max) };
    }
    case 'setChannelLayout':
      return { ...state, channelLayout: action.channelLayout };
    case 'setAmplitudeScale':
      return { ...state, amplitudeScale: action.amplitudeScale };
    case 'setFollowMode':
      return { ...state, followPlayhead: action.followPlayhead, followSuspended: false };
    case 'suspendFollow':
      return state.followPlayhead === 'off' ? state : { ...state, followSuspended: true };
    case 'resumeFollow':
      return { ...state, followSuspended: false };
    case 'setColorMap':
      return { ...state, spectrogram: { ...state.spectrogram, colorMap: action.colorMap } };
    case 'setSpectrogramRange':
      return {
        ...state,
        spectrogram: {
          ...state.spectrogram,
          displayFloorDb: action.displayFloorDb,
          gainDb: action.gainDb,
          gamma: Math.max(action.gamma, 0.01),
        },
      };
    case 'setFreqAxis':
      return { ...state, spectrogram: { ...state.spectrogram, freqAxis: action.freqAxis } };
    case 'setFftSizeOverride':
      return { ...state, spectrogram: { ...state.spectrogram, fftSizeOverride: action.fftSizeOverride } };
    case 'setDecimation':
      return { ...state, spectrogram: { ...state.spectrogram, decimation: action.decimation } };
  }
}

/** Any user-initiated pan/zoom suspends an active follow mode (M17 "must not fight user
 *  interaction") until the re-centre affordance calls `resumeFollow`. */
function applyFollowSuspendOnManualMove(state: ViewState, startFrame: number): ViewState {
  if (state.startFrame === startFrame) return state;
  const followSuspended = state.followPlayhead !== 'off' ? true : state.followSuspended;
  return { ...state, startFrame, followSuspended };
}

/** Thin mutable holder around `viewReducer` — the "owned outside the renderer" shared instance
 *  the app loop, interaction module, and any UI controls (zoom slider, layout picker) all read
 *  from and dispatch into. Not a general pub/sub store; M17's loop just re-reads `.state` once
 *  per rAF frame, so subscriptions would be unused machinery. */
export class ViewStateStore {
  private current: ViewState;

  constructor(initial: ViewState, public limits: ViewLimits) {
    this.current = initial;
  }

  get state(): ViewState {
    return this.current;
  }

  dispatch(action: ViewAction): ViewState {
    this.current = viewReducer(this.current, action, this.limits);
    return this.current;
  }

  setLimits(limits: ViewLimits): void {
    this.limits = limits;
    // Re-clamp against the new limits immediately (e.g. totalFrames just became known after a
    // decode finished) rather than waiting for the next user-driven action.
    this.current = viewReducer(this.current, { type: 'resize', widthCss: this.current.widthCss, heightCss: this.current.heightCss }, limits);
  }
}

/** Convenience for follow-mode driving code (the render loop): where should `startFrame` be so
 *  the playhead honours the current follow mode, given the frame it's currently at. Returns
 *  `null` when no adjustment is needed (mode is 'off', or the mode doesn't apply this frame). */
export function followTargetStartFrame(
  state: ViewState,
  playheadFrame: number,
  limits: ViewLimits,
): number | null {
  if (state.followPlayhead === 'off' || state.followSuspended) return null;
  const visibleFrames = state.framesPerPixel * state.widthCss;
  const relative = playheadFrame - state.startFrame;

  switch (state.followPlayhead) {
    case 'centre':
      return clampStartFrame(playheadFrame - visibleFrames / 2, state.framesPerPixel, limits, state.widthCss);
    case 'scroll': {
      // Keep the playhead within the middle 80% of the view; once it drifts past that band,
      // scroll continuously so it never reaches the edge.
      const margin = visibleFrames * 0.1;
      if (relative < margin) {
        return clampStartFrame(playheadFrame - margin, state.framesPerPixel, limits, state.widthCss);
      }
      if (relative > visibleFrames - margin) {
        return clampStartFrame(playheadFrame - (visibleFrames - margin), state.framesPerPixel, limits, state.widthCss);
      }
      return null;
    }
    case 'page':
      if (relative < 0 || relative > visibleFrames) {
        return clampStartFrame(playheadFrame, state.framesPerPixel, limits, state.widthCss);
      }
      return null;
  }
}
