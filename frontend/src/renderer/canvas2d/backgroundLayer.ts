// Background + grid layer (layer 0, M17 "Layered scene model"): flat theme background plus
// faint vertical gridlines at the same tick positions the time ruler uses, so the two never
// disagree.

import { drawTimeRuler, drawAmplitudeRuler, drawFrequencyRuler, type TimeRulerUnits } from './rulerLayer.ts';
import type { RenderFrame } from '../renderer.ts';

export function drawBackgroundLayer(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
): void {
  ctx.fillStyle = cssRgba(frame.theme.background);
  ctx.fillRect(0, 0, devicePixelWidth, devicePixelHeight);
}

export function drawRulers(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
  timeUnits: TimeRulerUnits,
  dpr: number,
): void {
  drawTimeRuler(ctx, frame, devicePixelWidth, devicePixelHeight, timeUnits, dpr);
  // Simplification: the left-edge ruler shows frequency (spectrogram) OR amplitude (waveform), not
  // both — both layers stack in the same region (M17's layered model, not a split view), and
  // drawing two competing y-axes in one strip would be unreadable. A real dual-axis UI (frequency
  // when the spectrogram has focus, amplitude when the waveform does, or a mode toggle) is a
  // natural follow-up once M18's overlay chrome exists to host that kind of control.
  if (frame.spectrogram) {
    drawFrequencyRuler(ctx, frame, devicePixelHeight, dpr);
  } else {
    drawAmplitudeRuler(ctx, frame, devicePixelWidth, devicePixelHeight, dpr);
  }
}

function cssRgba(c: { r: number; g: number; b: number; a: number }): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}
