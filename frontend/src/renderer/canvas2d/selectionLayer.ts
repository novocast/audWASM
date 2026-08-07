// Selection rendering with edge handles (M17 tasks list). Selection state itself is owned by the
// interaction module; this layer only draws whatever `RenderFrame.selection` currently holds.

import { frameToPixel } from '../coords.ts';
import type { RenderFrame } from '../renderer.ts';

export const kSelectionHandleWidthCss = 6;

export function drawSelectionLayer(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelHeight: number,
  dpr: number,
): void {
  if (!frame.selection) return;
  const { startFrame, endFrame } = frame.selection;
  const x1 = frameToPixel(Math.min(startFrame, endFrame), frame.view) * dpr;
  const x2 = frameToPixel(Math.max(startFrame, endFrame), frame.view) * dpr;
  if (x2 < 0 || x1 > ctx.canvas.width) return;

  const left = Math.max(x1, 0);
  const width = Math.min(x2, ctx.canvas.width) - left;
  if (width <= 0) return;

  ctx.fillStyle = cssRgba(frame.theme.selectionFill);
  ctx.fillRect(left, 0, width, devicePixelHeight);

  ctx.strokeStyle = cssRgba(frame.theme.selectionStroke);
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(Math.round(x1) + 0.5, 0);
  ctx.lineTo(Math.round(x1) + 0.5, devicePixelHeight);
  ctx.moveTo(Math.round(x2) + 0.5, 0);
  ctx.lineTo(Math.round(x2) + 0.5, devicePixelHeight);
  ctx.stroke();

  const handleW = kSelectionHandleWidthCss * dpr;
  ctx.fillStyle = cssRgba(frame.theme.selectionHandle);
  for (const x of [x1, x2]) {
    ctx.fillRect(x - handleW / 2, devicePixelHeight / 2 - handleW * 1.5, handleW, handleW * 3);
  }
}

function cssRgba(c: { r: number; g: number; b: number; a: number }): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}
