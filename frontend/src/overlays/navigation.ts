// M18 keyboard navigation: Tab/Shift+Tab (next/previous of the focused kind), `[`/`]` (next/
// previous of any kind), and jump-to-next-error. Pure functions over a MarkerStore snapshot so the
// interaction-layer wiring (mirroring interaction.ts's callback style) stays a thin adapter; every
// search wraps around the track rather than dead-ending at an edge, since "reachable by keyboard
// alone" (acceptance criteria) means an analyst scrubbing forward from anywhere always finds
// something rather than hitting a wall at the last marker.

import type { Marker, OverlayKind } from './model.ts';
import { lowerBound, upperBound, type MarkerStore } from './store.ts';

export type NavDirection = 'next' | 'previous';

function pickNearestPastOrWrap(sorted: readonly Marker[], fromFrame: number, direction: NavDirection): Marker | null {
  if (sorted.length === 0) return null;
  if (direction === 'next') {
    const idx = upperBound(sorted, fromFrame);
    return idx < sorted.length ? sorted[idx]! : sorted[0]!; // wrap to the first marker
  }
  const idx = lowerBound(sorted, fromFrame) - 1;
  return idx >= 0 ? sorted[idx]! : sorted[sorted.length - 1]!; // wrap to the last marker
}

/** `Tab`/`Shift+Tab`: next/previous marker of one specific kind (the "currently focused kind" —
 *  whichever lane/kind the user last interacted with; the caller tracks that, this just navigates
 *  within it). */
export function nextMarkerOfKind(store: MarkerStore, kind: OverlayKind, fromFrame: number, direction: NavDirection): Marker | null {
  return pickNearestPastOrWrap(store.get(kind), fromFrame, direction);
}

/** `[`/`]`: next/previous marker of *any* kind, optionally restricted to a set of currently-visible
 *  kinds (so navigating skips lanes the user has hidden). */
export function nextMarkerAnyKind(
  store: MarkerStore,
  fromFrame: number,
  direction: NavDirection,
  visibleKinds?: ReadonlySet<OverlayKind>,
): Marker | null {
  const all = store.all().filter((m) => !visibleKinds || visibleKinds.has(m.kind));
  return pickNearestPastOrWrap(all, fromFrame, direction);
}

/** The single prominent "jump to next error" action: searches `error` markers and `decoderEvent`
 *  markers with `severity === 'error'`, forward from `fromFrame`, wrapping to the first one in the
 *  track if nothing remains ahead. Returns `null` only when there are truly no errors at all. */
export function jumpToNextError(store: MarkerStore, fromFrame: number): Marker | null {
  const errors = [...store.get('error'), ...store.get('decoderEvent').filter((m) => m.severity === 'error')].sort(
    (a, b) => a.startFrame - b.startFrame,
  );
  return pickNearestPastOrWrap(errors, fromFrame, 'next');
}
