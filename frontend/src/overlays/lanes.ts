// M18 lane layout: ordering, collapse, resize, and persistence. "Decision — lane configuration is
// persisted per user, not per file, in localStorage" (task doc) — people develop a workflow layout
// and expect it back regardless of which track they open next.

import type { LaneId } from './model.ts';

export interface LaneConfig {
  id: LaneId;
  label: string;
  order: number;
  collapsed: boolean;
  /** CSS pixels, expanded height. Ignored (but retained) while collapsed. */
  heightCss: number;
}

const kStorageKey = 'aud.overlays.lanes.v1';

/** Default order/heights mirror the task doc's layout diagram: chapters/cues and beats sit above
 *  the waveform; loudness, transients and errors sit below it. The waveform itself isn't a lane
 *  here (silence/clipping/dc/selection draw inside the existing waveform layer, per the doc's "are
 *  drawn *in* here" distinction) — see model.ts's LaneId for the full set this excludes. */
export function defaultLaneConfigs(): LaneConfig[] {
  return [
    { id: 'chapters', label: 'Chapters / cue points', order: 0, collapsed: false, heightCss: 16 },
    { id: 'beats', label: 'Beats / bars', order: 1, collapsed: false, heightCss: 12 },
    { id: 'loudness', label: 'Loudness', order: 2, collapsed: false, heightCss: 40 },
    { id: 'transients', label: 'Transients / defects', order: 3, collapsed: false, heightCss: 12 },
    { id: 'lyrics', label: 'Lyrics', order: 4, collapsed: true, heightCss: 16 },
    { id: 'errors', label: 'Errors / decoder events', order: 5, collapsed: false, heightCss: 12 },
  ];
}

export interface LaneStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
}

/** Loads persisted lane config, filling in any lane the stored payload is missing (e.g. a newer
 *  build added a lane) with its default at the end of the order, and dropping any lane id the
 *  stored payload has that this build doesn't recognise (a downgrade, or a removed lane). */
export function loadLaneConfigs(storage: LaneStorage = safeLocalStorage()): LaneConfig[] {
  const defaults = defaultLaneConfigs();
  const raw = storage.getItem(kStorageKey);
  if (!raw) return defaults;

  try {
    const stored = JSON.parse(raw) as LaneConfig[];
    const byId = new Map(stored.map((l) => [l.id, l]));
    const known = new Set(defaults.map((l) => l.id));
    const merged = defaults.map((d) => byId.get(d.id) ?? d);
    // Preserve stored order for lanes still known, so a user's re-ordering round-trips.
    merged.sort((a, b) => a.order - b.order);
    return merged.filter((l) => known.has(l.id));
  } catch {
    return defaults; // corrupt payload: fail open to defaults rather than throwing at startup
  }
}

export function saveLaneConfigs(configs: readonly LaneConfig[], storage: LaneStorage = safeLocalStorage()): void {
  storage.setItem(kStorageKey, JSON.stringify(configs));
}

export function reorderLane(configs: readonly LaneConfig[], id: LaneId, newIndex: number): LaneConfig[] {
  const sorted = [...configs].sort((a, b) => a.order - b.order);
  const from = sorted.findIndex((l) => l.id === id);
  if (from < 0) return sorted;
  const [moved] = sorted.splice(from, 1);
  sorted.splice(clampIndex(newIndex, sorted.length), 0, moved!);
  return sorted.map((l, i) => ({ ...l, order: i }));
}

function clampIndex(i: number, length: number): number {
  return Math.min(Math.max(i, 0), length);
}

export function setLaneCollapsed(configs: readonly LaneConfig[], id: LaneId, collapsed: boolean): LaneConfig[] {
  return configs.map((l) => (l.id === id ? { ...l, collapsed } : l));
}

export function resizeLane(configs: readonly LaneConfig[], id: LaneId, heightCss: number): LaneConfig[] {
  const clamped = Math.max(6, heightCss);
  return configs.map((l) => (l.id === id ? { ...l, heightCss: clamped } : l));
}

/** Top-to-bottom pixel layout for the visible (non-collapsed-to-zero) lanes, in `order`. Collapsed
 *  lanes still get a thin strip (a header to re-expand) rather than vanishing entirely. */
export interface LaneRect {
  lane: LaneConfig;
  topCss: number;
  heightCss: number;
}

const kCollapsedStripHeightCss = 4;

export function layoutLanes(configs: readonly LaneConfig[]): LaneRect[] {
  const sorted = [...configs].sort((a, b) => a.order - b.order);
  let y = 0;
  const rects: LaneRect[] = [];
  for (const lane of sorted) {
    const heightCss = lane.collapsed ? kCollapsedStripHeightCss : lane.heightCss;
    rects.push({ lane, topCss: y, heightCss });
    y += heightCss;
  }
  return rects;
}

function safeLocalStorage(): LaneStorage {
  try {
    if (typeof localStorage !== 'undefined') return localStorage;
  } catch {
    /* SSR / disabled storage */
  }
  const memory = new Map<string, string>();
  return {
    getItem: (k) => memory.get(k) ?? null,
    setItem: (k, v) => memory.set(k, v),
  };
}
