// Builds and draws the waveform vertex buffer: one quad (2 triangles) per bin for the min/max
// envelope, plus one quad per bin for the RMS body, uploaded as a single buffer and drawn with a
// single drawArrays call per band (M17: "upload the queried bins ... and draw with a single ...
// call"). Only the 'summary' regime is drawn on the GPU path for this pass — 'sample'/'point' at
// extreme zoom-in are cheap enough (few hundred bins at most) that Canvas2D-quality polylines
// aren't worth a second GL code path yet; the WebGL backend falls back to those regimes looking
// identical to Canvas2D by simply not being selected as the active backend at that zoom (the
// backend-selection module can prefer Canvas2D once framesPerPixel drops below 'summary').

import { amplitudeToUnit, layoutChannelBands, waveformQueryBinCount, type RenderFrame, type WaveformBinsLike } from '../renderer.ts';
import { createProgram } from './glUtil.ts';
import { kWaveformFragmentShader, kWaveformVertexShader } from './shaders.ts';

export class WaveformProgram {
  private readonly gl: WebGL2RenderingContext;
  private readonly program: WebGLProgram;
  private readonly vao: WebGLVertexArrayObject;
  private readonly buffer: WebGLBuffer;
  private readonly uEnvelopeColor: WebGLUniformLocation | null;
  private readonly uRmsColor: WebGLUniformLocation | null;
  private vertexData = new Float32Array(0);

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    this.program = createProgram(gl, kWaveformVertexShader, kWaveformFragmentShader);
    this.uEnvelopeColor = gl.getUniformLocation(this.program, 'uEnvelopeColor');
    this.uRmsColor = gl.getUniformLocation(this.program, 'uRmsColor');

    const vao = gl.createVertexArray();
    const buffer = gl.createBuffer();
    if (!vao || !buffer) throw new Error('WaveformProgram: failed to allocate VAO/buffer');
    this.vao = vao;
    this.buffer = buffer;

    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    const stride = 3 * 4; // x, y, isRms (float32)
    const aPosition = gl.getAttribLocation(this.program, 'aPosition');
    const aIsRms = gl.getAttribLocation(this.program, 'aIsRms');
    gl.enableVertexAttribArray(aPosition);
    gl.vertexAttribPointer(aPosition, 2, gl.FLOAT, false, stride, 0);
    gl.enableVertexAttribArray(aIsRms);
    gl.vertexAttribPointer(aIsRms, 1, gl.FLOAT, false, stride, 2 * 4);
    gl.bindVertexArray(null);
  }

  /** Rebuilds GL objects after context restoration — callers must re-`new` this class, since
   *  every WebGL object handle from before the loss is invalid. Kept as a static note rather than
   *  a method: there is nothing to "restore" on an existing instance. */

  draw(frame: RenderFrame, canvasWidthPx: number, canvasHeightPx: number, colors: { envelope: [number, number, number, number]; rms: [number, number, number, number] }): void {
    const gl = this.gl;
    if (!frame.waveform) return;
    const { bins, channels } = frame.waveform;
    const binCount = waveformQueryBinCount(frame.waveform);
    // Shared with the Canvas2D backend (M17 follow-up "mid/side and overlaid layout polish") so
    // both backends land on the exact same integer-device-pixel band boundaries.
    const bands = layoutChannelBands(frame.view.channelLayout, channels, canvasHeightPx);

    // 6 vertices/quad * 2 quads (envelope+rms) * binCount * bandsToDraw, 3 floats/vertex.
    const floatsNeeded = bands.length * binCount * 2 * 6 * 3;
    if (this.vertexData.length !== floatsNeeded) this.vertexData = new Float32Array(floatsNeeded);
    const data = this.vertexData;
    let o = 0;

    const pxToClipX = (px: number): number => (px / canvasWidthPx) * 2 - 1;
    const pxToClipY = (px: number): number => 1 - (px / canvasHeightPx) * 2;

    for (const band of bands) {
      const binOffset = band.channelIndex * binCount;
      const midYpx = band.topDevicePx + band.heightDevicePx / 2;
      const scale = (band.heightDevicePx / 2) * frame.view.verticalZoom;
      const n = Math.min(binCount, canvasWidthPx);

      for (let x = 0; x < n; x++) {
        const i = binOffset + x;
        o = emitBinQuads(data, o, x, x + 1, midYpx, scale, bins, i, pxToClipX, pxToClipY, frame);
      }
    }

    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    gl.bufferData(gl.ARRAY_BUFFER, data.subarray(0, o), gl.DYNAMIC_DRAW);
    gl.useProgram(this.program);
    gl.uniform4fv(this.uEnvelopeColor, colors.envelope);
    gl.uniform4fv(this.uRmsColor, colors.rms);
    gl.drawArrays(gl.TRIANGLES, 0, o / 3);
    gl.bindVertexArray(null);
  }

  dispose(): void {
    const gl = this.gl;
    gl.deleteBuffer(this.buffer);
    gl.deleteVertexArray(this.vao);
    gl.deleteProgram(this.program);
  }
}

function emitQuad(
  data: Float32Array,
  o: number,
  x0: number,
  x1: number,
  y0: number,
  y1: number,
  isRms: number,
): number {
  // Two triangles: (x0,y0)-(x1,y0)-(x0,y1) and (x1,y0)-(x1,y1)-(x0,y1).
  const verts: [number, number][] = [
    [x0, y0],
    [x1, y0],
    [x0, y1],
    [x1, y0],
    [x1, y1],
    [x0, y1],
  ];
  for (const [x, y] of verts) {
    data[o++] = x;
    data[o++] = y;
    data[o++] = isRms;
  }
  return o;
}

function emitBinQuads(
  data: Float32Array,
  o: number,
  xPx0: number,
  xPx1: number,
  midYpx: number,
  scale: number,
  bins: WaveformBinsLike,
  binIndex: number,
  pxToClipX: (px: number) => number,
  pxToClipY: (px: number) => number,
  frame: RenderFrame,
): number {
  const maxUnit = amplitudeToUnit(bins.max(binIndex), frame.view.amplitudeScale);
  const minUnit = amplitudeToUnit(bins.min(binIndex), frame.view.amplitudeScale);
  const rms = Math.abs(amplitudeToUnit(bins.rms(binIndex), frame.view.amplitudeScale));

  const yTopPx = midYpx - maxUnit * scale;
  const yBottomPx = midYpx - minUnit * scale;
  const yRmsTopPx = midYpx - rms * scale;
  const yRmsBottomPx = midYpx + rms * scale;

  const cx0 = pxToClipX(xPx0);
  const cx1 = pxToClipX(xPx1);
  o = emitQuad(data, o, cx0, cx1, pxToClipY(Math.min(yTopPx, yBottomPx)), pxToClipY(Math.max(yTopPx, yBottomPx)), 0);
  o = emitQuad(data, o, cx0, cx1, pxToClipY(yRmsTopPx), pxToClipY(yRmsBottomPx), 1);
  return o;
}
