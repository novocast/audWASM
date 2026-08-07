// Time ruler (adaptive tick density, units toggle) and amplitude ruler (M17 "Text rendering":
// rulers and all text live in a Canvas2D layer above the WebGL canvas, always — a glyph atlas is
// too much machinery for text that changes rarely).
//
// TODO(M18/M13): a 'bars+beats' unit mode needs a beat grid from M13, which doesn't exist yet;
// the units toggle only supports 'time' and 'samples' for now, with the type left open for it.

import { frameToPixel, frameToSeconds, hzToPixel } from '../coords.ts';
import type { RenderFrame } from '../renderer.ts';

export type TimeRulerUnits = 'time' | 'samples' | 'bars-beats';

const kTargetTickSpacingPx = 80;
// "Nice" tick step candidates in seconds, spanning sub-millisecond to hours.
const kNiceSecondSteps = [
  0.0001, 0.0002, 0.0005, 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15,
  30, 60, 120, 300, 600, 900, 1800, 3600, 7200,
];

function chooseSecondsStep(secondsPerPixel: number): number {
  const target = secondsPerPixel * kTargetTickSpacingPx;
  for (const step of kNiceSecondSteps) {
    if (step >= target) return step;
  }
  return kNiceSecondSteps[kNiceSecondSteps.length - 1]!;
}

function chooseFrameStep(framesPerPixel: number): number {
  const target = framesPerPixel * kTargetTickSpacingPx;
  const pow2 = Math.pow(2, Math.ceil(Math.log2(Math.max(target, 1))));
  return pow2;
}

function formatSeconds(seconds: number, step: number): string {
  const sign = seconds < 0 ? '-' : '';
  const abs = Math.abs(seconds);
  const h = Math.floor(abs / 3600);
  const m = Math.floor((abs % 3600) / 60);
  const s = abs % 60;
  const decimals = step < 1 ? Math.min(3, Math.max(0, Math.ceil(-Math.log10(step)))) : 0;
  const sStr = s.toFixed(decimals).padStart(decimals > 0 ? 3 + decimals : 2, '0');
  if (h > 0) return `${sign}${h}:${String(m).padStart(2, '0')}:${sStr}`;
  return `${sign}${m}:${sStr}`;
}

export function drawTimeRuler(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
  units: TimeRulerUnits,
  dpr: number,
): void {
  ctx.save();
  ctx.font = `${11 * dpr}px system-ui, sans-serif`;
  ctx.textBaseline = 'top';
  ctx.strokeStyle = cssRgba(frame.theme.rulerTick);
  ctx.fillStyle = cssRgba(frame.theme.rulerText);
  ctx.lineWidth = 1;

  const tickPath = new Path2D();

  if (units === 'samples') {
    const step = chooseFrameStep(frame.view.framesPerPixel);
    const firstFrame = Math.floor(frame.view.startFrame / step) * step;
    for (let f = firstFrame; ; f += step) {
      const x = frameToPixel(f, frame.view) * dpr;
      if (x > devicePixelWidth) break;
      if (x >= 0) {
        tickPath.moveTo(Math.round(x) + 0.5, 0);
        tickPath.lineTo(Math.round(x) + 0.5, 6 * dpr);
        ctx.fillText(String(Math.round(f)), x + 2 * dpr, 2 * dpr);
      }
    }
  } else {
    const secondsPerPixel = frame.view.framesPerPixel / frame.sampleRate;
    const step = chooseSecondsStep(secondsPerPixel);
    const startSeconds = frameToSeconds(frame.view.startFrame, frame.sampleRate);
    const firstTick = Math.floor(startSeconds / step) * step;
    for (let t = firstTick; ; t += step) {
      const frameAt = t * frame.sampleRate;
      const x = frameToPixel(frameAt, frame.view) * dpr;
      if (x > devicePixelWidth) break;
      if (x >= 0) {
        tickPath.moveTo(Math.round(x) + 0.5, 0);
        tickPath.lineTo(Math.round(x) + 0.5, 6 * dpr);
        ctx.fillText(formatSeconds(t, step), x + 2 * dpr, 2 * dpr);
      }
    }
  }

  ctx.stroke(tickPath);
  ctx.restore();
}

export function drawAmplitudeRuler(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelWidth: number,
  devicePixelHeight: number,
  dpr: number,
): void {
  ctx.save();
  ctx.font = `${10 * dpr}px system-ui, sans-serif`;
  ctx.textBaseline = 'middle';
  ctx.strokeStyle = cssRgba(frame.theme.rulerTick);
  ctx.fillStyle = cssRgba(frame.theme.rulerText);

  const isDb = frame.view.amplitudeScale.type === 'db';
  const labels = isDb ? [0, -6, -12, -24, -48] : [1, 0.5, 0, -0.5, -1];
  const midY = devicePixelHeight / 2;
  const scale = (devicePixelHeight / 2) * frame.view.verticalZoom;

  const tickPath = new Path2D();
  for (const label of labels) {
    const unit = isDb ? (label === 0 ? 1 : Math.pow(10, label / 20)) : label;
    const y = Math.round(midY - unit * scale) + 0.5;
    tickPath.moveTo(0, y);
    tickPath.lineTo(6 * dpr, y);
    ctx.fillText(isDb ? `${label}dB` : label.toFixed(1), 8 * dpr, y);
  }
  ctx.stroke(tickPath);
  ctx.restore();
}

// "Nice" Hz candidates for a linear axis, same 1-2-5-per-decade spirit as chooseSecondsStep.
const kNiceHzSteps = [
  1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000,
];

function chooseLinearHzStep(hzPerPixel: number): number {
  const target = hzPerPixel * kTargetTickSpacingPx;
  for (const step of kNiceHzSteps) {
    if (step >= target) return step;
  }
  return kNiceHzSteps[kNiceHzSteps.length - 1]!;
}

// Standard "1-2-5 per decade" reference frequencies audio tools tick a log axis at (20Hz-100kHz
// covers every axis this app supports with margin) — a log/mel/bark axis has a fixed, bounded
// overall span rather than continuously varying like the time axis, so enumerating the whole
// candidate set and filtering to what's in range is simpler and reads better than a "choose one
// step" function (M07 "Frequency ruler with log-aware tick placement (octaves...)").
const kLogHzCandidates = [
  20, 30, 50, 70, 100, 200, 300, 500, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000, 30000, 50000,
];

function formatHz(hz: number): string {
  if (hz >= 1000) {
    const khz = hz / 1000;
    return `${khz % 1 === 0 ? khz.toFixed(0) : khz.toFixed(1)}k`;
  }
  return String(Math.round(hz));
}

/** Frequency (y) ruler for the spectrogram (M07 "Frequency ruler with log-aware tick placement").
 *  Drawn across the full device-pixel height passed in — for a per-channel-band ruler in split
 *  layouts, the caller should invoke this once per band with that band's height/offset instead.
 */
export function drawFrequencyRuler(
  ctx: CanvasRenderingContext2D,
  frame: RenderFrame,
  devicePixelHeight: number,
  dpr: number,
): void {
  if (!frame.spectrogram) return;

  const axis = frame.view.spectrogram.freqAxis;
  const nyquistHz = frame.sampleRate / 2;
  const minHz = axis === 'linear' ? 0 : 20;
  const params = { axis, minHz, maxHz: nyquistHz, heightCss: devicePixelHeight };

  ctx.save();
  ctx.font = `${10 * dpr}px system-ui, sans-serif`;
  ctx.textBaseline = 'middle';
  ctx.strokeStyle = cssRgba(frame.theme.rulerTick);
  ctx.fillStyle = cssRgba(frame.theme.rulerText);

  const tickPath = new Path2D();
  const drawTick = (hz: number): void => {
    if (hz < minHz || hz > nyquistHz) return;
    const y = Math.round(hzToPixel(hz, params) * dpr) + 0.5;
    tickPath.moveTo(0, y);
    tickPath.lineTo(6 * dpr, y);
    ctx.fillText(`${formatHz(hz)}Hz`, 8 * dpr, y);
  };

  if (axis === 'linear') {
    const step = chooseLinearHzStep((nyquistHz / devicePixelHeight) * dpr);
    for (let hz = 0; hz <= nyquistHz; hz += step) drawTick(hz);
  } else {
    for (const hz of kLogHzCandidates) drawTick(hz);
  }

  ctx.stroke(tickPath);
  ctx.restore();
}

function cssRgba(c: { r: number; g: number; b: number; a: number }): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}
