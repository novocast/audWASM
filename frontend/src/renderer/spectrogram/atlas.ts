// Texture atlas allocation for spectrogram tiles (M07 "a texture atlas, not one texture per tile" —
// WebGL texture binds/allocations are expensive; one 4096x4096 R8 atlas holds 256 fixed-size
// 256x256 slots and needs exactly one bind per draw).
//
// Decision — fixed-size slots, a plain free list. Every tile is exactly 256x256 (kTileWidth x
// kTileHeight), so there is no variable-size packing problem and therefore no fragmentation
// possible by construction (M07 risk table) — allocation is either "pop the free list" or "evict
// the least-recently-touched slot", never a bin-packing search.

export const kAtlasTileSize = 256; // == kTileWidth == kTileHeight
export const kAtlasSize = 4096; // 16x16 slots of 256x256 = 16MB R8
export const kAtlasSlotsPerSide = kAtlasSize / kAtlasTileSize;
export const kAtlasSlotCount = kAtlasSlotsPerSide * kAtlasSlotsPerSide;

export interface AtlasSlotRect {
  /** Normalised [0,1] UV rect within the atlas texture. */
  u0: number;
  v0: number;
  u1: number;
  v1: number;
}

export class TileAtlas {
  readonly texture: WebGLTexture;

  private readonly gl: WebGL2RenderingContext;
  private freeSlots: number[] = [];
  private slotOf = new Map<string, number>();
  private keyOfSlot: (string | null)[] = new Array(kAtlasSlotCount).fill(null);
  // Recency order, oldest first — a plain array is fine at <=256 entries touched at most a few
  // times per frame; this is not a hot per-pixel path.
  private recency: string[] = [];

  constructor(gl: WebGL2RenderingContext) {
    this.gl = gl;
    const texture = gl.createTexture();
    if (!texture) throw new Error('TileAtlas: failed to allocate texture');
    this.texture = texture;

    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, kAtlasSize, kAtlasSize, 0, gl.RED, gl.UNSIGNED_BYTE, null);
    // LINEAR for smooth zoom between columns/rows; CLAMP_TO_EDGE so adjacent slots never bleed
    // into one another at a tile's edge.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);

    for (let i = kAtlasSlotCount - 1; i >= 0; i--) this.freeSlots.push(i);
  }

  hasSlot(key: string): boolean {
    return this.slotOf.has(key);
  }

  slotRectFor(key: string): AtlasSlotRect | null {
    const slot = this.slotOf.get(key);
    return slot === undefined ? null : this.rectForSlot(slot);
  }

  /** Uploads `bytes` (kAtlasTileSize*kAtlasTileSize) for `key`, allocating (or evicting the LRU
   *  slot) if it isn't already resident, and returns its UV rect. Touches (marks most-recently-used)
   *  an already-resident slot without re-uploading — callers should only call this when `bytes` is
   *  actually new (see program.ts's dedupe-by-reference check), since texSubImage2D isn't free. */
  upload(key: string, bytes: Uint8Array): AtlasSlotRect {
    let slot = this.slotOf.get(key);
    if (slot === undefined) {
      slot = this.allocateSlot(key);
    } else {
      this.touch(key);
    }

    const gl = this.gl;
    const { x, y } = this.pixelCoordsForSlot(slot);
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, x, y, kAtlasTileSize, kAtlasTileSize, gl.RED, gl.UNSIGNED_BYTE, bytes);
    gl.bindTexture(gl.TEXTURE_2D, null);

    return this.rectForSlot(slot);
  }

  private allocateSlot(key: string): number {
    let slot: number;
    if (this.freeSlots.length > 0) {
      slot = this.freeSlots.pop()!;
    } else {
      const evictKey = this.recency.shift();
      if (evictKey === undefined) throw new Error('TileAtlas: no free slot and nothing to evict');
      slot = this.slotOf.get(evictKey)!;
      this.slotOf.delete(evictKey);
      this.keyOfSlot[slot] = null;
    }
    this.slotOf.set(key, slot);
    this.keyOfSlot[slot] = key;
    this.recency.push(key);
    return slot;
  }

  private touch(key: string): void {
    const i = this.recency.indexOf(key);
    if (i >= 0) {
      this.recency.splice(i, 1);
      this.recency.push(key);
    }
  }

  private pixelCoordsForSlot(slot: number): { x: number; y: number } {
    const col = slot % kAtlasSlotsPerSide;
    const row = Math.floor(slot / kAtlasSlotsPerSide);
    return { x: col * kAtlasTileSize, y: row * kAtlasTileSize };
  }

  private rectForSlot(slot: number): AtlasSlotRect {
    const { x, y } = this.pixelCoordsForSlot(slot);
    return {
      u0: x / kAtlasSize,
      v0: y / kAtlasSize,
      u1: (x + kAtlasTileSize) / kAtlasSize,
      v1: (y + kAtlasTileSize) / kAtlasSize,
    };
  }

  dispose(): void {
    this.gl.deleteTexture(this.texture);
  }
}
