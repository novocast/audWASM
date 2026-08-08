// Full Canvas2D implementation of every layer except the spectrogram (M17 "Canvas2D is not a
// second-class citizen"). One stacked `<canvas>` per layer (0: background+rulers, 2: waveform,
// 3: selection, 4: overlays) so a dirty waveform never forces a redraw of the ruler sitting above
// it — each layer's dirty flag maps directly to "does this specific canvas get cleared+redrawn
// this frame", which is also what makes the dirty-counter assertions in M17's acceptance
// criteria ("the playhead moving does not trigger a waveform redraw") meaningful for this backend.
//
// Layer 1 (spectrogram) is WebGL-only per M17's design and is intentionally absent here.

import { kLayerOrder, DirtyTracker, type Layer, type Renderer, type RenderFrame, type RenderStats } from '../renderer.ts';
import type { ThemeTokens } from '../theme.ts';
import { drawBackgroundLayer, drawRulers } from './backgroundLayer.ts';
import { drawWaveformLayer } from './waveformLayer.ts';
import { drawSelectionLayer } from './selectionLayer.ts';
import { drawOverlaysLayer } from '../overlays/drawOverlays.ts';
import type { TimeRulerUnits } from './rulerLayer.ts';

const kCanvasLayers: readonly Layer[] = ['background', 'waveform', 'selection', 'overlays'];

export class Canvas2DRenderer implements Renderer {
  readonly backend = 'canvas2d' as const;
  readonly dirty = new DirtyTracker();

  private container: HTMLElement | null = null;
  private readonly canvases = new Map<Layer, HTMLCanvasElement>();
  private readonly contexts = new Map<Layer, CanvasRenderingContext2D>();
  private widthCss = 0;
  private heightCss = 0;
  private devicePixelRatio = 1;
  private theme: ThemeTokens | null = null;
  timeRulerUnits: TimeRulerUnits = 'time';
  /** Hit candidates from the most recent overlays redraw — interaction.ts's click handler reads
   *  this via `hitCandidates()` rather than recomputing density itself. */
  private lastHitCandidates: import('../../overlays/hitTest.ts').HitCandidate[] = [];

  hitCandidates(): readonly import('../../overlays/hitTest.ts').HitCandidate[] {
    return this.lastHitCandidates;
  }

  attach(container: HTMLElement): void {
    this.container = container;
    container.style.position = 'relative';
    for (const layer of kCanvasLayers) {
      const canvas = document.createElement('canvas');
      canvas.style.position = 'absolute';
      canvas.style.inset = '0';
      canvas.style.pointerEvents = 'none';
      const ctx = canvas.getContext('2d');
      if (!ctx) throw new Error(`Canvas2DRenderer: 2d context unavailable for layer "${layer}"`);
      this.canvases.set(layer, canvas);
      this.contexts.set(layer, ctx);
      container.appendChild(canvas);
    }
    this.dirty.markAllDirty();
  }

  resize(widthCss: number, heightCss: number, devicePixelRatio: number): void {
    this.widthCss = widthCss;
    this.heightCss = heightCss;
    this.devicePixelRatio = devicePixelRatio;
    for (const canvas of this.canvases.values()) {
      canvas.style.width = `${widthCss}px`;
      canvas.style.height = `${heightCss}px`;
      canvas.width = Math.max(1, Math.round(widthCss * devicePixelRatio));
      canvas.height = Math.max(1, Math.round(heightCss * devicePixelRatio));
    }
    this.dirty.markAllDirty();
  }

  setTheme(theme: ThemeTokens): void {
    this.theme = theme;
    this.dirty.markAllDirty();
  }

  render(frame: RenderFrame): RenderStats {
    const t0 = performance.now();
    const dpr = this.devicePixelRatio;
    const dw = Math.max(1, Math.round(this.widthCss * dpr));
    const dh = Math.max(1, Math.round(this.heightCss * dpr));
    const layersRedrawn: Layer[] = [];

    if (this.dirty.consumeDirty('background')) {
      const ctx = this.contexts.get('background')!;
      ctx.clearRect(0, 0, dw, dh);
      drawBackgroundLayer(ctx, frame, dw, dh);
      drawRulers(ctx, frame, dw, dh, this.timeRulerUnits, dpr);
      layersRedrawn.push('background');
    }

    if (this.dirty.consumeDirty('waveform')) {
      const ctx = this.contexts.get('waveform')!;
      ctx.clearRect(0, 0, dw, dh);
      drawWaveformLayer(ctx, frame, dw, dh);
      layersRedrawn.push('waveform');
    }

    if (this.dirty.consumeDirty('selection')) {
      const ctx = this.contexts.get('selection')!;
      ctx.clearRect(0, 0, dw, dh);
      drawSelectionLayer(ctx, frame, dh, dpr);
      layersRedrawn.push('selection');
    }

    if (this.dirty.consumeDirty('overlays')) {
      const ctx = this.contexts.get('overlays')!;
      ctx.clearRect(0, 0, dw, dh);
      this.lastHitCandidates = drawOverlaysLayer(ctx, frame, dw, dh, dpr).hitCandidates;
      layersRedrawn.push('overlays');
    }

    return { frameTimeMs: performance.now() - t0, layersRedrawn };
  }

  dispose(): void {
    for (const canvas of this.canvases.values()) canvas.remove();
    this.canvases.clear();
    this.contexts.clear();
    this.container = null;
  }
}

export { kLayerOrder };
