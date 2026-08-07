// The playhead + interaction-feedback overlay (layers 5-6, M17's "single most important
// performance decision"): a small, separate Canvas2D canvas that redraws every frame while
// playing, so a moving cursor never forces a redraw of the (unchanged) waveform underneath. Always
// Canvas2D, regardless of which main Renderer backend is active.

import { frameToPixel } from './coords.ts';
import type { ViewState } from './viewState.ts';
import type { ThemeTokens } from './theme.ts';

export interface HoverFeedback {
  xCss: number;
  /** CSS-pixel y-coordinate within the host element — added for M07's spectrogram cursor readout
   *  (frequency depends on where vertically the cursor is, not just when). Optional since the
   *  playhead overlay itself only ever needs xCss. */
  yCss?: number;
  label?: string;
}

export interface PlayheadOverlayFrame {
  view: ViewState;
  theme: ThemeTokens;
  playheadFrame: number | null;
  hover: HoverFeedback | null;
  /** Ghost cursor shown while dragging a selection edge, in source frames. */
  dragGhostFrame: number | null;
}

export class PlayheadOverlay {
  private readonly canvas: HTMLCanvasElement;
  private readonly ctx: CanvasRenderingContext2D;
  private widthCss = 0;
  private heightCss = 0;
  private devicePixelRatio = 1;

  constructor(container: HTMLElement) {
    this.canvas = document.createElement('canvas');
    this.canvas.style.position = 'absolute';
    this.canvas.style.inset = '0';
    this.canvas.style.pointerEvents = 'none';
    const ctx = this.canvas.getContext('2d');
    if (!ctx) throw new Error('PlayheadOverlay: 2d context unavailable');
    this.ctx = ctx;
    container.appendChild(this.canvas);
  }

  resize(widthCss: number, heightCss: number, devicePixelRatio: number): void {
    this.widthCss = widthCss;
    this.heightCss = heightCss;
    this.devicePixelRatio = devicePixelRatio;
    this.canvas.style.width = `${widthCss}px`;
    this.canvas.style.height = `${heightCss}px`;
    this.canvas.width = Math.max(1, Math.round(widthCss * devicePixelRatio));
    this.canvas.height = Math.max(1, Math.round(heightCss * devicePixelRatio));
  }

  /** Cheap enough to call unconditionally every rAF frame while anything is moving — this is the
   *  whole point of keeping it on its own canvas (M17: "~0.1ms/frame instead of a full re-render"). */
  render(frame: PlayheadOverlayFrame): void {
    const dpr = this.devicePixelRatio;
    const dw = Math.max(1, Math.round(this.widthCss * dpr));
    const dh = Math.max(1, Math.round(this.heightCss * dpr));
    const ctx = this.ctx;
    ctx.clearRect(0, 0, dw, dh);

    if (frame.playheadFrame !== null) {
      const x = frameToPixel(frame.playheadFrame, frame.view) * dpr;
      if (x >= 0 && x <= dw) {
        ctx.strokeStyle = cssRgba(frame.theme.cursor);
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(Math.round(x) + 0.5, 0);
        ctx.lineTo(Math.round(x) + 0.5, dh);
        ctx.stroke();
      }
    }

    if (frame.dragGhostFrame !== null) {
      const x = frameToPixel(frame.dragGhostFrame, frame.view) * dpr;
      if (x >= 0 && x <= dw) {
        ctx.save();
        ctx.setLineDash([4 * dpr, 4 * dpr]);
        ctx.strokeStyle = cssRgba({ ...frame.theme.cursor, a: frame.theme.cursor.a * 0.6 });
        ctx.beginPath();
        ctx.moveTo(Math.round(x) + 0.5, 0);
        ctx.lineTo(Math.round(x) + 0.5, dh);
        ctx.stroke();
        ctx.restore();
      }
    }

    if (frame.hover) {
      const x = frame.hover.xCss * dpr;
      ctx.fillStyle = cssRgba({ ...frame.theme.cursor, a: 0.15 });
      ctx.fillRect(x - dpr, 0, 2 * dpr, dh);
      if (frame.hover.label) {
        ctx.font = `${11 * dpr}px system-ui, sans-serif`;
        ctx.textBaseline = 'top';
        ctx.fillStyle = cssRgba(frame.theme.rulerText);
        ctx.fillText(frame.hover.label, x + 4 * dpr, 4 * dpr);
      }
    }
  }

  dispose(): void {
    this.canvas.remove();
  }
}

function cssRgba(c: { r: number; g: number; b: number; a: number }): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}
