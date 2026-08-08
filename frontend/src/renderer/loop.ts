// The single app-level rAF loop (M17 "The render loop" — "One requestAnimationFrame loop for the
// whole app. No per-component rAF."). Owns: reading the playback position, driving follow modes,
// querying the engine for waveform data when the view or waveform data changed, redrawing dirty
// layers on the main Renderer, and unconditionally redrawing the cheap playhead overlay.
//
// Deliberately does not import bindings/wasm/engine.ts — `queryWaveform` is supplied by the
// caller so this module (and its tests) never need a WASM module instance.

import type { Renderer, RenderFrame, WaveformQueryResult, MarkerLike, SelectionRange, SpectrogramSource, OverlayFrameData } from './renderer.ts';
import type { ThemeTokens } from './theme.ts';
import { readThemeTokens, watchThemeChanges } from './theme.ts';
import { watchDevicePixelRatio } from './dprWatcher.ts';
import { followTargetStartFrame, type ViewStateStore } from './viewState.ts';
import { PlayheadOverlay, type HoverFeedback } from './playheadOverlay.ts';
import { FrameTimeStats, type FrameStatsSnapshot } from './frameStats.ts';

export interface RenderLoopOptions {
  hostElement: HTMLElement;
  renderer: Renderer;
  viewStore: ViewStateStore;
  getPlayheadFrame(): number | null;
  isPlaying(): boolean;
  sampleRate(): number;
  trackDurationFrames(): number;
  /** binCountDevicePx is the device-pixel width of the view — the app loop always requests
   *  pixel-width-sized data (M17 "Decision — engine data queries happen synchronously"). */
  queryWaveform(view: import('./viewState.ts').ViewState, binCountDevicePx: number): WaveformQueryResult | null;
  getSelection(): SelectionRange | null;
  getMarkers(): readonly MarkerLike[];
  /** M18's overlay data (marker store view, lane config, curve series, selection highlight). Null
   *  (the default when omitted) until the host wires up an overlays store — the 'overlays' layer
   *  then just clears itself each frame, same as before M18 landed. */
  getOverlayFrame?(): OverlayFrameData | null;
  /** Null until the spectrogram worker has loaded PCM for the current track (M07). Reads the tile
   *  manager's current state — does not itself trigger tile requests (the caller's tile manager
   *  does that from its own per-frame update, driven by `invalidateSpectrogram`'s trigger point). */
  getSpectrogramSource?(): SpectrogramSource | null;
  onFrameStats?(stats: FrameStatsSnapshot): void;
  /** Called first, every frame, before follow-mode/waveform/render logic — the seam that keeps
   *  this the *only* rAF loop in the app (M17: "No per-component rAF"). Playback position ticking
   *  and any other per-frame UI updates the host page needs belong here rather than in a second
   *  requestAnimationFrame callback. */
  onBeforeFrame?(nowMs: number): void;
}

export class RenderLoop {
  readonly overlay: PlayheadOverlay;
  readonly frameStats = new FrameTimeStats();

  private rafHandle: number | null = null;
  private resizeObserver: ResizeObserver;
  private unwatchTheme: () => void;
  private unwatchDpr: () => void;
  private theme: ThemeTokens;
  private hoverFeedback: HoverFeedback | null = null;
  private dragGhostFrame: number | null = null;
  private lastWaveformKey = '';
  private cachedWaveform: WaveformQueryResult | null = null;
  private lastViewRef: unknown = null;

  constructor(private readonly opts: RenderLoopOptions) {
    this.overlay = new PlayheadOverlay(opts.hostElement);
    this.theme = readThemeTokens(opts.hostElement);
    opts.renderer.setTheme(this.theme);

    this.resizeObserver = new ResizeObserver((entries) => {
      const entry = entries[0];
      if (!entry) return;
      this.handleResize(entry.contentRect.width, entry.contentRect.height);
    });
    this.resizeObserver.observe(opts.hostElement);
    this.handleResize(opts.hostElement.clientWidth, opts.hostElement.clientHeight);

    this.unwatchTheme = watchThemeChanges(() => {
      this.theme = readThemeTokens(opts.hostElement);
      opts.renderer.setTheme(this.theme);
      opts.renderer.dirty.markAllDirty();
    });
    this.unwatchDpr = watchDevicePixelRatio((dpr) => {
      const state = opts.viewStore.dispatch({ type: 'setDpr', devicePixelRatio: dpr });
      opts.renderer.resize(state.widthCss, state.heightCss, dpr);
      this.overlay.resize(state.widthCss, state.heightCss, dpr);
    });
  }

  private handleResize(widthCss: number, heightCss: number): void {
    const dpr = window.devicePixelRatio || 1;
    const state = this.opts.viewStore.dispatch({ type: 'resize', widthCss, heightCss });
    this.opts.viewStore.dispatch({ type: 'setDpr', devicePixelRatio: dpr });
    this.opts.renderer.resize(state.widthCss, state.heightCss, dpr);
    this.overlay.resize(state.widthCss, state.heightCss, dpr);
    this.opts.renderer.dirty.markAllDirty();
  }

  setHover(hover: HoverFeedback | null): void {
    this.hoverFeedback = hover;
  }

  setDragGhost(frame: number | null): void {
    this.dragGhostFrame = frame;
  }

  /** Any view/data change that should force a waveform requery+redraw calls this — the
   *  interaction module and playback position updates both funnel through it rather than each
   *  independently deciding when the waveform layer is stale. */
  invalidateWaveform(): void {
    this.opts.renderer.dirty.markDirty('waveform');
  }

  invalidateSelection(): void {
    this.opts.renderer.dirty.markDirty('selection');
  }

  /** New/changed markers, a lane reorder/collapse/resize, or a new inspector selection — anything
   *  that changes what the overlays layer should draw next frame without the view itself moving. */
  invalidateOverlays(): void {
    this.opts.renderer.dirty.markDirty('overlays');
  }

  /** New tiles arrived, or the visible set changed — the spectrogram worker's `tile`/`overview`
   *  messages and the tile manager's own per-frame viewport recompute both funnel through this. */
  invalidateSpectrogram(): void {
    this.opts.renderer.dirty.markDirty('spectrogram');
  }

  /** Forces an immediate theme re-read and full redraw — the same work `watchThemeChanges`'s
   *  media-query listener triggers, exposed for a manual theme toggle (a `data-theme` override
   *  doesn't fire `prefers-color-scheme`/`prefers-contrast` media query change events, so nothing
   *  else would prompt a re-read). */
  refreshTheme(): void {
    this.theme = readThemeTokens(this.opts.hostElement);
    this.opts.renderer.setTheme(this.theme);
    this.opts.renderer.dirty.markAllDirty();
  }

  start(): void {
    if (this.rafHandle !== null) return;
    const tick = (nowMs: number): void => {
      this.renderFrame(nowMs);
      this.rafHandle = requestAnimationFrame(tick);
    };
    this.rafHandle = requestAnimationFrame(tick);
  }

  stop(): void {
    if (this.rafHandle !== null) cancelAnimationFrame(this.rafHandle);
    this.rafHandle = null;
  }

  private renderFrame(nowMs: number): void {
    const { opts } = this;
    opts.onBeforeFrame?.(nowMs);
    const playheadFrame = opts.getPlayheadFrame();
    const isPlaying = opts.isPlaying();

    if (playheadFrame !== null) {
      const target = followTargetStartFrame(opts.viewStore.state, playheadFrame, opts.viewStore.limits);
      if (target !== null) {
        opts.viewStore.dispatch({ type: 'followMove', startFrame: target });
        opts.renderer.dirty.markAllDirty();
      }
    }

    const view = opts.viewStore.state;
    if (view !== this.lastViewRef) {
      // Any view change (pan/zoom/channel layout/amplitude scale/...) moves or reshapes the
      // ruler ticks and the selection rectangle even when neither's own data changed — both are
      // pure functions of ViewState, so a reducer dispatch from anywhere (interaction.ts, a UI
      // control, follow-mode tracking) must invalidate them here rather than relying on each call
      // site to remember to. The waveform layer has its own finer-grained key below (it also
      // needs to redraw when new decode data arrives with no view change at all).
      opts.renderer.dirty.markDirty('background');
      opts.renderer.dirty.markDirty('selection');
      // A view change (pan/zoom) also moves every marker/lane pixel position, same reasoning as
      // the ruler/selection above.
      opts.renderer.dirty.markDirty('overlays');
      // A view change (pan/zoom) changes which tiles are visible — the tile manager itself
      // recomputes on its own per-frame tick, but the *draw* needs to happen this frame too, not
      // just whenever the next tile happens to arrive.
      opts.renderer.dirty.markDirty('spectrogram');
      this.lastViewRef = view;
    }

    const devicePixelWidth = Math.max(1, Math.round(view.widthCss * view.devicePixelRatio));
    const waveformKey = `${view.startFrame}:${view.framesPerPixel}:${view.channelLayout}:${devicePixelWidth}`;
    if (waveformKey !== this.lastWaveformKey) {
      this.cachedWaveform = opts.queryWaveform(view, devicePixelWidth);
      this.lastWaveformKey = waveformKey;
      opts.renderer.dirty.markDirty('waveform');
    }

    const frame: RenderFrame = {
      view,
      limits: opts.viewStore.limits,
      sampleRate: opts.sampleRate(),
      trackDurationFrames: opts.trackDurationFrames(),
      waveform: this.cachedWaveform,
      playheadFrame,
      isPlaying,
      selection: opts.getSelection(),
      markers: opts.getMarkers(),
      overlays: opts.getOverlayFrame?.() ?? null,
      theme: this.theme,
      spectrogram: opts.getSpectrogramSource?.() ?? null,
    };

    opts.renderer.render(frame);

    this.overlay.render({
      view,
      theme: this.theme,
      playheadFrame,
      hover: this.hoverFeedback,
      dragGhostFrame: this.dragGhostFrame,
    });

    // Measured across the whole frame (main renderer + overlay), not just the main renderer's
    // own RenderStats — the overlay redraws unconditionally every frame and is part of the
    // budget M17's acceptance criteria (p99 < 16.6ms) actually cares about.
    this.frameStats.record(performance.now() - nowMs);
    opts.onFrameStats?.(this.frameStats.snapshot());
  }

  dispose(): void {
    this.stop();
    this.resizeObserver.disconnect();
    this.unwatchTheme();
    this.unwatchDpr();
    this.overlay.dispose();
  }
}
