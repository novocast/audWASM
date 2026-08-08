// M18 user-created markers (bookmarks/annotations): create/edit/delete, keyed to the source hash
// so they survive re-analysis and follow the file rather than a session.
//
// Persistence seam: the task doc says these belong "in the cache (M16) in their own chunk, keyed
// to the source hash". M16 (`documentation/tasks/M16-waveform-cache-format.md`) is still in
// progress and has no frontend `.awc` read/write binding yet, so this module persists to
// `localStorage` under the same source-hash key an M16 chunk would use. Swapping the storage
// backend later is a matter of replacing `BookmarkStorage`'s implementation — every call site here
// already goes through it rather than touching `localStorage` directly — once M16 exposes a chunk
// API on the TS side. `.awc` export/import and CSV/JSON (exportMarkers.ts) already work against
// plain `Marker[]`, so nothing above this module needs to change when that swap happens.

import type { Marker } from './model.ts';
import type { MarkerStore } from './store.ts';

export interface BookmarkStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

function keyFor(sourceHash: string): string {
  return `aud.overlays.bookmarks.v1.${sourceHash}`;
}

function safeLocalStorage(): BookmarkStorage {
  try {
    if (typeof localStorage !== 'undefined') return localStorage;
  } catch {
    /* SSR / disabled storage */
  }
  const memory = new Map<string, string>();
  return {
    getItem: (k) => memory.get(k) ?? null,
    setItem: (k, v) => memory.set(k, v),
    removeItem: (k) => memory.delete(k),
  };
}

let nextId = 0;
/** Monotonic-enough id generator: bookmark ids only need to be unique within one loaded track's
 *  session, and export/import round-trips the id verbatim rather than regenerating it. */
function makeBookmarkId(): string {
  nextId += 1;
  return `bookmark-${Date.now()}-${nextId}`;
}

export function loadBookmarks(sourceHash: string, storage: BookmarkStorage = safeLocalStorage()): Marker[] {
  const raw = storage.getItem(keyFor(sourceHash));
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw) as Marker[];
    return parsed.map((m) => ({ ...m, userCreated: true }));
  } catch {
    return []; // corrupt payload: fail open rather than blocking the track from loading
  }
}

export function saveBookmarks(sourceHash: string, markers: readonly Marker[], storage: BookmarkStorage = safeLocalStorage()): void {
  storage.setItem(keyFor(sourceHash), JSON.stringify(markers));
}

/** Creates a bookmark, adds it to `store`, and persists the full updated bookmark set for
 *  `sourceHash`. Returns the created marker (its generated `id` included) so a caller (e.g. the
 *  inspector, right after `M`-key creation) can immediately select/focus it. */
export function createBookmark(
  store: MarkerStore,
  sourceHash: string,
  startFrame: number,
  label?: string,
  storage?: BookmarkStorage,
): Marker {
  const marker: Marker = { id: makeBookmarkId(), kind: 'bookmark', startFrame, userCreated: true, ...(label !== undefined ? { label } : {}) };
  store.add(marker);
  saveBookmarks(sourceHash, store.get('bookmark'), storage);
  return marker;
}

export function updateBookmark(
  store: MarkerStore,
  sourceHash: string,
  id: string,
  patch: Partial<Marker>,
  storage?: BookmarkStorage,
): Marker | null {
  const updated = store.update(id, 'bookmark', { ...patch, userCreated: true });
  if (updated) saveBookmarks(sourceHash, store.get('bookmark'), storage);
  return updated;
}

export function deleteBookmark(store: MarkerStore, sourceHash: string, id: string, storage?: BookmarkStorage): boolean {
  const removed = store.remove(id, 'bookmark');
  if (removed) saveBookmarks(sourceHash, store.get('bookmark'), storage);
  return removed;
}

/** Loads `sourceHash`'s persisted bookmarks into `store` — call once when a track is opened,
 *  before any analyser results arrive, so `replaceAnalysisMarkers` never has a chance to run
 *  before the user's own markers are present (it only ever replaces non-`userCreated` entries, but
 *  loading order still matters for anything that reads the store in between). */
export function hydrateBookmarks(store: MarkerStore, sourceHash: string, storage?: BookmarkStorage): void {
  for (const m of loadBookmarks(sourceHash, storage)) store.add(m);
}
