// WebGL2 Renderer: layers 1 (spectrogram, M07) and 2 (waveform) on a GL canvas; layers 0/3/4
// (background+rulers, selection, overlays) on a stacked Canvas2D canvas
// reusing the exact same draw functions the Canvas2D backend uses, so the two backends produce
// visually equivalent output for everything except the spectrogram, by construction rather than
// by careful duplication (M17 acceptance criteria).

import { DirtyTracker, type Layer, type Renderer, type RenderFrame, type RenderStats } from '../renderer.ts';
import type { ThemeTokens } from '../theme.ts';
import { drawBackgroundLayer, drawRulers } from '../canvas2d/backgroundLayer.ts';
import { drawSelectionLayer } from '../canvas2d/selectionLayer.ts';
import type { TimeRulerUnits } from '../canvas2d/rulerLayer.ts';
import { watchContextLoss } from './glUtil.ts';
import { WaveformProgram } from './waveformProgram.ts';
import { SpectrogramProgram } from '../spectrogram/program.ts';

export class WebglRenderer implements Renderer {
  readonly backend = 'webgl2' as const;
  readonly dirty = new DirtyTracker();

  private container: HTMLElement | null = null;
  private glCanvas!: HTMLCanvasElement;
  private overlayCanvas!: HTMLCanvasElement;
  private overlayCtx!: CanvasRenderingContext2D;
  private gl: WebGL2RenderingContext | null = null;
  private waveformProgram: WaveformProgram | null = null;
  private spectrogramProgram: SpectrogramProgram | null = null;
  private unwatchContextLoss: (() => void) | null = null;
  private contextLost = false;
  private widthCss = 0;
  private heightCss = 0;
  private devicePixelRatio = 1;
  private theme: ThemeTokens | null = null;
  timeRulerUnits: TimeRulerUnits = 'time';

  /** Set by the caller after construction if it wants to know to fall back to Canvas2D on
   *  repeated context loss (M17 risk table). Not auto-wired here since the fallback is a
   *  backend-selection decision the app owns, not something this class should do to itself. */
  onContextLossExceeded: (() => void) | null = null;
  private contextLossCount = 0;

  attach(container: HTMLElement): void {
    this.container = container;
    container.style.position = 'relative';

    this.glCanvas = document.createElement('canvas');
    this.glCanvas.style.position = 'absolute';
    this.glCanvas.style.inset = '0';
    this.glCanvas.style.pointerEvents = 'none';
    container.appendChild(this.glCanvas);

    this.overlayCanvas = document.createElement('canvas');
    this.overlayCanvas.style.position = 'absolute';
    this.overlayCanvas.style.inset = '0';
    this.overlayCanvas.style.pointerEvents = 'none';
    container.appendChild(this.overlayCanvas);
    const overlayCtx = this.overlayCanvas.getContext('2d');
    if (!overlayCtx) throw new Error('WebglRenderer: 2d context unavailable for overlay canvas');
    this.overlayCtx = overlayCtx;

    this.initGl();
    this.unwatchContextLoss = watchContextLoss(this.glCanvas, {
      onLost: () => {
        this.contextLost = true;
        this.contextLossCount += 1;
        this.waveformProgram = null;
        this.spectrogramProgram = null;
        this.gl = null;
        if (this.contextLossCount > 3) this.onContextLossExceeded?.();
      },
      onRestored: () => {
        this.contextLost = false;
        this.initGl();
        this.dirty.markAllDirty();
      },
    });

    this.dirty.markAllDirty();
  }

  private initGl(): void {
    const gl = this.glCanvas.getContext('webgl2', { alpha: true, antialias: true });
    if (!gl) throw new Error('WebglRenderer: failed to acquire a webgl2 context');
    this.gl = gl;
    this.waveformProgram = new WaveformProgram(gl);
    this.spectrogramProgram = new SpectrogramProgram(gl);
  }

  resize(widthCss: number, heightCss: number, devicePixelRatio: number): void {
    this.widthCss = widthCss;
    this.heightCss = heightCss;
    this.devicePixelRatio = devicePixelRatio;
    for (const canvas of [this.glCanvas, this.overlayCanvas]) {
      canvas.style.width = `${widthCss}px`;
      canvas.style.height = `${heightCss}px`;
      canvas.width = Math.max(1, Math.round(widthCss * devicePixelRatio));
      canvas.height = Math.max(1, Math.round(heightCss * devicePixelRatio));
    }
    this.gl?.viewport(0, 0, this.glCanvas.width, this.glCanvas.height);
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

    // Background, spectrogram, and waveform share one physical GL canvas/framebuffer (layers 0's
    // fill, 1's tiles, 2's bars), so any one being dirty forces a full clear+redraw of that canvas
    // — there's no way to touch up just one layer under already-drawn ones on a single buffer.
    // Each flag still only counts as "redrawn" in the dirty tracker when it was actually dirty,
    // which is what the layer-redraw-counter assertions care about. Draw order matches
    // kLayerOrder: spectrogram under the waveform.
    const backgroundWasDirty = this.dirty.consumeDirty('background');
    const spectrogramWasDirty = this.dirty.consumeDirty('spectrogram');
    const waveformWasDirty = this.dirty.consumeDirty('waveform');
    if (
      !this.contextLost &&
      this.gl &&
      this.waveformProgram &&
      this.spectrogramProgram &&
      (backgroundWasDirty || spectrogramWasDirty || waveformWasDirty)
    ) {
      const gl = this.gl;
      const bg = frame.theme.background;
      gl.clearColor(bg.r, bg.g, bg.b, bg.a);
      gl.clear(gl.COLOR_BUFFER_BIT);
      this.spectrogramProgram.draw(frame, dw, dh);
      const t = frame.theme;
      this.waveformProgram.draw(frame, dw, dh, {
        envelope: [t.waveformFill.r, t.waveformFill.g, t.waveformFill.b, t.waveformFill.a],
        rms: [t.waveformRms.r, t.waveformRms.g, t.waveformRms.b, t.waveformRms.a],
      });
      if (backgroundWasDirty) layersRedrawn.push('background');
      if (spectrogramWasDirty) layersRedrawn.push('spectrogram');
      if (waveformWasDirty) layersRedrawn.push('waveform');
    }

    let overlayTouched = false;
    if (backgroundWasDirty) {
      this.overlayCtx.clearRect(0, 0, dw, dh);
      // GL canvas paints the background fill; the 2D overlay only adds ticks/labels on top.
      drawRulers(this.overlayCtx, frame, dw, dh, this.timeRulerUnits, dpr);
      overlayTouched = true;
    }
    if (this.dirty.consumeDirty('selection')) {
      if (!overlayTouched) this.overlayCtx.clearRect(0, 0, dw, dh);
      drawSelectionLayer(this.overlayCtx, frame, dh, dpr);
      layersRedrawn.push('selection');
    }
    if (this.dirty.consumeDirty('overlays')) {
      layersRedrawn.push('overlays'); // M18 territory; nothing to draw yet.
    }

    if (this.contextLost) {
      // Keep the background visible via the 2D canvas alone while the GL context is down.
      drawBackgroundLayer(this.overlayCtx, frame, dw, dh);
    }

    return { frameTimeMs: performance.now() - t0, layersRedrawn };
  }

  dispose(): void {
    this.unwatchContextLoss?.();
    this.waveformProgram?.dispose();
    this.spectrogramProgram?.dispose();
    this.glCanvas?.remove();
    this.overlayCanvas?.remove();
    this.container = null;
  }
}
