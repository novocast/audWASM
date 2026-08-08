// M18's hit testing: pixel-tolerant (a 1px marker needs a ~6px hit target) with deterministic
// overlap resolution — "topmost lane first, then nearest, then by a fixed kind priority" (task
// doc's "Hit testing and the inspector"). Operates on plain candidate records rather than the
// store directly so it stays trivially unit-testable and agnostic of how a caller built the list
// (canvas draw pass, a findings-list row, a test fixture).

import { metaFor, type Marker } from './model.ts';

export interface HitCandidate {
  marker: Marker;
  /** Device- or CSS-pixel x position (point marker) or span (region) — caller's choice of unit,
   *  as long as `toleranceUnits` and every candidate share it. */
  pixelStart: number;
  pixelEnd: number;
  /** Lane draw order, topmost first (index 0 = topmost). Ties within the same lane are broken by
   *  distance then priority; ties across lanes always prefer the lower index. */
  laneOrder: number;
}

function distanceToSpan(px: number, start: number, end: number): number {
  if (px < start) return start - px;
  if (px > end) return px - end;
  return 0;
}

/**
 * Returns the winning candidate at `px`, or `null` if nothing is within `toleranceUnits` of it.
 * Deterministic tie-break order: topmost lane (`laneOrder` ascending) first, then nearest pixel
 * distance, then highest kind priority (errors > defects > user markers > analysis markers, per
 * model.ts's registry), then insertion order as a final, fully deterministic fallback.
 */
export function hitTestMarkers(candidates: readonly HitCandidate[], px: number, toleranceUnits: number): Marker | null {
  let best: HitCandidate | null = null;
  let bestDistance = Infinity;

  // A plain loop, not Array#forEach: `best`'s later reassignment need only be visible to the next
  // iteration, and insertion order (the final, fully deterministic tie-break) falls out of
  // iterating candidates in the order the caller built them — an earlier candidate never loses a
  // tie to a later one under `compareCandidates`' strict `<` comparison below.
  for (const candidate of candidates) {
    const distance = distanceToSpan(px, candidate.pixelStart, candidate.pixelEnd);
    if (distance > toleranceUnits) continue;
    if (!best || compareCandidates(candidate, distance, best, bestDistance) < 0) {
      best = candidate;
      bestDistance = distance;
    }
  }

  return best ? best.marker : null;
}

/** Negative => `a` wins over `b`. */
function compareCandidates(a: HitCandidate, aDistance: number, b: HitCandidate, bDistance: number): number {
  if (a.laneOrder !== b.laneOrder) return a.laneOrder - b.laneOrder;
  if (aDistance !== bDistance) return aDistance - bDistance;
  const aPriority = metaFor(a.marker.kind).priority;
  const bPriority = metaFor(b.marker.kind).priority;
  return bPriority - aPriority; // higher priority wins => sorts first
}
