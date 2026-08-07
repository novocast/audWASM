// Builds and draws the spectrogram: the eager overview strip (stretched across the full track,
// drawn first so there's never empty space) plus every visible fine tile on top, sampling a shared
// texture atlas (M07 "Tile pipeline": "shader samples atlas + colour-map LUT texture"). Same
// constructor/draw()/dispose() shape as webgl/waveformProgram.ts, but textured-quad (UV) geometry
// instead of vertex-colour line/bar geometry.

import { frameToPixel, cssToDevicePixel } from '../coords.ts';
import { layoutChannelBands, type RenderFrame, type SpectrogramTileRef } from '../renderer.ts';
import { createProgram } from '../webgl/glUtil.ts';
import { kSpectrogramFragmentShader, kSpectrogramVertexShader } from './shader.ts';
import { TileAtlas, kAtlasTileSize } from './atlas.ts';
import { buildColorMapLut, type SpectrogramColorMapName } from './colormaps.ts';
import { hopForLevel, foldFactorForLevel } from './tileManager.ts';

function tileKeyString(ref: SpectrogramTileRef): string {
  return `${ref.level}:${ref.tileX}:${ref.channel}`;
}

export class SpectrogramProgram {
  private readonly gl: WebGL2RenderingContext;
  private readonly program: WebGLProgram;
  private readonly vao: WebGLVertexArrayObject;
  private readonly buffer: WebGLBuffer;
  private readonly atlas: TileAtlas;
  private readonly lutTexture: WebGLTexture;
  private readonly overviewTextures = new Map<number, WebGLTexture>();

  private readonly uSource: WebGLUniformLocation | null;
  private readonly uColorMap: WebGLUniformLocation | null;
  private readonly uFloorDb: WebGLUniformLocation | null;
  private readonly uCeilDb: WebGLUniformLocation | null;
  private readonly uDisplayFloorDb: WebGLUniformLocation | null;
  private readonly uGainDb: WebGLUniformLocation | null;
  private readonly uGamma: WebGLUniformLocation | null;

  // Dedupe-by-reference: only re-upload a tile/overview's bytes into GPU memory when the object
  // reference actually changed (a new tile arrived), never unconditionally every frame.
  private uploadedTileBytes = new Map<string, Uint8Array>();
  private uploadedOverviewBytes = new Map<number, Uint8Array>();
  private currentColorMap: SpectrogramColorMapName | null = null;

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    this.program = createProgram(gl, kSpectrogramVertexShader, kSpectrogramFragmentShader);

    this.uSource = gl.getUniformLocation(this.program, 'uSource');
    this.uColorMap = gl.getUniformLocation(this.program, 'uColorMap');
    this.uFloorDb = gl.getUniformLocation(this.program, 'uFloorDb');
    this.uCeilDb = gl.getUniformLocation(this.program, 'uCeilDb');
    this.uDisplayFloorDb = gl.getUniformLocation(this.program, 'uDisplayFloorDb');
    this.uGainDb = gl.getUniformLocation(this.program, 'uGainDb');
    this.uGamma = gl.getUniformLocation(this.program, 'uGamma');

    const vao = gl.createVertexArray();
    const buffer = gl.createBuffer();
    if (!vao || !buffer) throw new Error('SpectrogramProgram: failed to allocate VAO/buffer');
    this.vao = vao;
    this.buffer = buffer;

    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    const stride = 4 * 4; // x, y, u, v (float32)
    const aPosition = gl.getAttribLocation(this.program, 'aPosition');
    const aUV = gl.getAttribLocation(this.program, 'aUV');
    gl.enableVertexAttribArray(aPosition);
    gl.vertexAttribPointer(aPosition, 2, gl.FLOAT, false, stride, 0);
    gl.enableVertexAttribArray(aUV);
    gl.vertexAttribPointer(aUV, 2, gl.FLOAT, false, stride, 2 * 4);
    gl.bindVertexArray(null);

    this.atlas = new TileAtlas(gl);

    const lutTexture = gl.createTexture();
    if (!lutTexture) throw new Error('SpectrogramProgram: failed to allocate LUT texture');
    this.lutTexture = lutTexture;
  }

  draw(frame: RenderFrame, canvasWidthPx: number, canvasHeightPx: number): void {
    const gl = this.gl;
    const source = frame.spectrogram;
    if (!source) return;

    this.ensureColorMap(frame.view.spectrogram.colorMap);

    const trackChannelCount = frame.waveform?.channels ?? 1;
    const bands = layoutChannelBands(frame.view.channelLayout, trackChannelCount, canvasHeightPx);
    const dpr = frame.view.devicePixelRatio;

    gl.useProgram(this.program);
    gl.uniform1f(this.uFloorDb, -96);
    gl.uniform1f(this.uCeilDb, 0);
    gl.uniform1f(this.uDisplayFloorDb, frame.view.spectrogram.displayFloorDb);
    gl.uniform1f(this.uGainDb, frame.view.spectrogram.gainDb);
    gl.uniform1f(this.uGamma, frame.view.spectrogram.gamma);
    gl.uniform1i(this.uSource, 0);
    gl.uniform1i(this.uColorMap, 1);

    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.lutTexture);

    gl.bindVertexArray(this.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);

    // Pass 1: overview strips, one per channel band, stretched across the full track duration —
    // always drawn first so there is never empty space while fine tiles are still streaming in
    // (M07 "The overview level").
    for (const band of bands) {
      const overview = source.overviewBytes(band.channelIndex);
      if (!overview) continue;
      const texture = this.ensureOverviewTexture(band.channelIndex, overview);

      const x0 = cssToDevicePixel(frameToPixel(0, frame.view), dpr);
      const x1 = cssToDevicePixel(frameToPixel(frame.trackDurationFrames, frame.view), dpr);
      const data = new Float32Array(
        this.quad(x0, x1, band.topDevicePx, band.topDevicePx + band.heightDevicePx, 0, 0, 1, 1, canvasWidthPx, canvasHeightPx),
      );

      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.uniform1f(this.uFloorDb, overview.floorDb);
      gl.uniform1f(this.uCeilDb, overview.ceilDb);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
    }

    // Pass 2: every visible fine tile, one shared atlas bind for the whole pass.
    const visible = source.visibleTiles();
    const quads: number[] = [];
    let tileFloorDb = -96;
    let tileCeilDb = 0;

    for (const ref of visible) {
      const bytes = source.tileBytes(ref);
      if (!bytes) continue;
      const key = tileKeyString(ref);

      const previous = this.uploadedTileBytes.get(key);
      let rect = this.atlas.slotRectFor(key);
      if (previous !== bytes.bytes || !rect) {
        rect = this.atlas.upload(key, bytes.bytes);
        this.uploadedTileBytes.set(key, bytes.bytes);
      }

      const band = bands.find((b) => b.channelIndex === ref.channel);
      if (!band) continue;

      // The fftSize this *specific* tile was generated at (not necessarily what the view's
      // current zoom would pick right now — fftSize is adaptive, see tileManager.ts) is what its
      // true time span depends on; using anything else would misplace the quad.
      const fftSize = bytes.fftSize;
      const hop = hopForLevel(fftSize, ref.level);
      const fold = foldFactorForLevel(ref.level);
      const tileSpanSamples = kAtlasTileSize * hop * fold;

      const x0 = cssToDevicePixel(frameToPixel(ref.tileX * tileSpanSamples, frame.view), dpr);
      const x1 = cssToDevicePixel(frameToPixel((ref.tileX + 1) * tileSpanSamples, frame.view), dpr);
      const y0 = band.topDevicePx;
      const y1 = band.topDevicePx + band.heightDevicePx;

      quads.push(...this.quad(x0, x1, y0, y1, rect.u0, rect.v0, rect.u1, rect.v1, canvasWidthPx, canvasHeightPx));
      tileFloorDb = bytes.floorDb;
      tileCeilDb = bytes.ceilDb;
    }

    if (quads.length > 0) {
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.atlas.texture);
      gl.uniform1f(this.uFloorDb, tileFloorDb);
      gl.uniform1f(this.uCeilDb, tileCeilDb);
      const data = new Float32Array(quads);
      gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
      gl.drawArrays(gl.TRIANGLES, 0, data.length / 4);
    }

    gl.bindVertexArray(null);
  }

  private ensureColorMap(name: SpectrogramColorMapName): void {
    if (this.currentColorMap === name) return;
    this.currentColorMap = name;
    const lut = buildColorMapLut(name);
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.lutTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, lut);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  private ensureOverviewTexture(channel: number, overview: { bytes: Uint8Array; width: number; height: number }): WebGLTexture {
    const gl = this.gl;
    let texture = this.overviewTextures.get(channel);
    const isNew = !texture;
    if (!texture) {
      const created = gl.createTexture();
      if (!created) throw new Error('SpectrogramProgram: failed to allocate overview texture');
      texture = created;
      this.overviewTextures.set(channel, texture);
    }

    if (isNew || this.uploadedOverviewBytes.get(channel) !== overview.bytes) {
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, overview.width, overview.height, 0, gl.RED, gl.UNSIGNED_BYTE, overview.bytes);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.bindTexture(gl.TEXTURE_2D, null);
      this.uploadedOverviewBytes.set(channel, overview.bytes);
    }

    return texture;
  }

  private quad(
    x0Device: number,
    x1Device: number,
    y0Device: number,
    y1Device: number,
    u0: number,
    v0: number,
    u1: number,
    v1: number,
    canvasWidthPx: number,
    canvasHeightPx: number,
  ): number[] {
    const cx0 = (x0Device / canvasWidthPx) * 2 - 1;
    const cx1 = (x1Device / canvasWidthPx) * 2 - 1;
    const cy0 = 1 - (y0Device / canvasHeightPx) * 2;
    const cy1 = 1 - (y1Device / canvasHeightPx) * 2;

    // Two triangles, matching waveformProgram.ts's emitQuad winding.
    return [
      cx0, cy0, u0, v0,
      cx1, cy0, u1, v0,
      cx0, cy1, u0, v1,
      cx1, cy0, u1, v0,
      cx1, cy1, u1, v1,
      cx0, cy1, u0, v1,
    ];
  }

  dispose(): void {
    const gl = this.gl;
    gl.deleteBuffer(this.buffer);
    gl.deleteVertexArray(this.vao);
    gl.deleteProgram(this.program);
    gl.deleteTexture(this.lutTexture);
    for (const texture of this.overviewTextures.values()) gl.deleteTexture(texture);
    this.atlas.dispose();
  }
}
