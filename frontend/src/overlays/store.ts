// M18's marker store: one sorted-by-`startFrame` array per kind, so the render/hit-test/navigation
// hot paths are O(visible) via binary search rather than O(total) (task doc's "Performance"
// section). Also the single place that keeps `userCreated` markers distinct enough to survive
// re-analysis: `replaceAnalysisMarkers` only ever touches non-user markers of the given kind.

import type { Marker, OverlayKind } from './model.ts';

function compareByStart(a: Marker, b: Marker): number {
  return a.startFrame - b.startFrame;
}

/** Lowest index `i` such that `arr[i].startFrame >= value` (a standard lower_bound). Isolated as
 *  its own function so the boundary cases — empty array, value below/above every element, value
 *  equal to an element — have one implementation to get right and one place to unit test. */
export function lowerBound(arr: readonly Marker[], value: number): number {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (arr[mid]!.startFrame < value) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/** Lowest index `i` such that `arr[i].startFrame > value` (upper_bound). */
export function upperBound(arr: readonly Marker[], value: number): number {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (arr[mid]!.startFrame <= value) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/**
 * Markers visible in `[startFrame, endFrame]`, by two binary searches plus a linear scan of any
 * region markers whose `startFrame` sorts before the window but whose `endFrame` still reaches
 * into it (a region can start off-screen and still be partially visible). The scan skips point
 * markers (they have no `endFrame` to extend their reach, so a point sorting before the window is
 * never visible) and stops at the first *region* it finds fully behind the window — regions are
 * typically few relative to point kinds and, in practice, both start and end monotonically with
 * position (silence gaps, chapters), so "first region behind the window" is a reasonable place to
 * stop rather than walking every earlier marker. Dense point kinds (clipping/transients) never pay
 * for this scan at all, since a `break`/`continue` on them is O(1).
 */
export function visibleSlice(sorted: readonly Marker[], startFrame: number, endFrame: number): Marker[] {
  const from = lowerBound(sorted, startFrame);
  const to = upperBound(sorted, endFrame);
  const result = sorted.slice(from, to);

  for (let i = from - 1; i >= 0; i--) {
    const m = sorted[i]!;
    if (m.endFrame === undefined) continue; // point marker sorting before the window: never visible, keep scanning
    if (m.endFrame < startFrame) break; // this region is fully behind the window
    result.unshift(m);
  }
  return result;
}

export class MarkerStore {
  private readonly byKind = new Map<OverlayKind, Marker[]>();

  /** Replaces every non-user-created marker of `kind` with `markers` (re-sorted). Used when an
   *  analyser (re-)runs — user bookmarks of the same kind, if any ever exist, are untouched. */
  replaceAnalysisMarkers(kind: OverlayKind, markers: readonly Marker[]): void {
    const kept = (this.byKind.get(kind) ?? []).filter((m) => m.userCreated);
    const next = [...kept, ...markers.filter((m) => !m.userCreated)];
    next.sort(compareByStart);
    this.byKind.set(kind, next);
  }

  add(marker: Marker): void {
    const arr = this.byKind.get(marker.kind) ?? [];
    const idx = lowerBound(arr, marker.startFrame);
    arr.splice(idx, 0, marker);
    this.byKind.set(marker.kind, arr);
  }

  remove(id: string, kind: OverlayKind): boolean {
    const arr = this.byKind.get(kind);
    if (!arr) return false;
    const idx = arr.findIndex((m) => m.id === id);
    if (idx < 0) return false;
    arr.splice(idx, 1);
    return true;
  }

  update(id: string, kind: OverlayKind, patch: Partial<Marker>): Marker | null {
    const arr = this.byKind.get(kind);
    if (!arr) return null;
    const idx = arr.findIndex((m) => m.id === id);
    if (idx < 0) return null;
    const updated = { ...arr[idx]!, ...patch, id, kind };
    arr.splice(idx, 1);
    const insertAt = lowerBound(arr, updated.startFrame);
    arr.splice(insertAt, 0, updated);
    return updated;
  }

  get(kind: OverlayKind): readonly Marker[] {
    return this.byKind.get(kind) ?? [];
  }

  kinds(): OverlayKind[] {
    return [...this.byKind.keys()];
  }

  visible(kind: OverlayKind, startFrame: number, endFrame: number): Marker[] {
    return visibleSlice(this.byKind.get(kind) ?? [], startFrame, endFrame);
  }

  /** Every marker of every kind, sorted by start frame — the findings list's data source. */
  all(): Marker[] {
    return [...this.byKind.values()].flat().sort(compareByStart);
  }

  byId(id: string): Marker | null {
    for (const arr of this.byKind.values()) {
      const found = arr.find((m) => m.id === id);
      if (found) return found;
    }
    return null;
  }

  clear(): void {
    this.byKind.clear();
  }
}
