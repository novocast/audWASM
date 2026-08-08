import { describe, expect, it } from 'vitest';
import { lowerBound, upperBound, visibleSlice, MarkerStore } from './store.ts';
import type { Marker } from './model.ts';

function m(id: string, startFrame: number, endFrame?: number): Marker {
  return { id, kind: 'clipping', startFrame, ...(endFrame !== undefined ? { endFrame } : {}) };
}

describe('lowerBound/upperBound', () => {
  it('return 0 on an empty array', () => {
    expect(lowerBound([], 5)).toBe(0);
    expect(upperBound([], 5)).toBe(0);
  });

  it('handle a single-element array on both sides of the value', () => {
    const arr = [m('a', 10)];
    expect(lowerBound(arr, 5)).toBe(0);
    expect(lowerBound(arr, 10)).toBe(0);
    expect(lowerBound(arr, 15)).toBe(1);
    expect(upperBound(arr, 10)).toBe(1);
    expect(upperBound(arr, 9)).toBe(0);
  });

  it('find exact-match boundaries among duplicates', () => {
    const arr = [m('a', 1), m('b', 5), m('c', 5), m('d', 5), m('e', 9)];
    expect(lowerBound(arr, 5)).toBe(1);
    expect(upperBound(arr, 5)).toBe(4);
  });

  it('clamp to array length when the value is past every element', () => {
    const arr = [m('a', 1), m('b', 2)];
    expect(lowerBound(arr, 100)).toBe(2);
    expect(upperBound(arr, 100)).toBe(2);
  });
});

describe('visibleSlice', () => {
  it('returns nothing for an empty array', () => {
    expect(visibleSlice([], 0, 100)).toEqual([]);
  });

  it('returns the single element only when the window covers it', () => {
    const arr = [m('a', 50)];
    expect(visibleSlice(arr, 0, 100).map((x) => x.id)).toEqual(['a']);
    expect(visibleSlice(arr, 60, 100)).toEqual([]);
    expect(visibleSlice(arr, 0, 40)).toEqual([]);
  });

  it('includes a region marker whose start is before the window but whose end reaches into it', () => {
    const arr = [m('a', 0, 20), m('b', 50, 60)];
    expect(visibleSlice(arr, 10, 100).map((x) => x.id)).toEqual(['a', 'b']);
  });

  it('excludes a point marker sorting before the window (no endFrame to extend it)', () => {
    const arr = [m('a', 5), m('b', 50)];
    expect(visibleSlice(arr, 10, 100).map((x) => x.id)).toEqual(['b']);
  });

  it('a point marker before the window does not block an earlier region from being found', () => {
    const arr = [m('region', 0, 20), m('point', 5), m('inWindow', 50, 60)];
    expect(visibleSlice(arr, 10, 100).map((x) => x.id)).toEqual(['region', 'inWindow']);
  });

  it('is exact at the window boundaries (inclusive)', () => {
    const arr = [m('a', 10), m('b', 20), m('c', 30)];
    expect(visibleSlice(arr, 10, 20).map((x) => x.id)).toEqual(['a', 'b']);
  });
});

describe('MarkerStore', () => {
  it('replaceAnalysisMarkers keeps userCreated markers and drops stale analysis ones', () => {
    const store = new MarkerStore();
    store.add({ id: 'user-1', kind: 'bookmark', startFrame: 5, userCreated: true });
    store.replaceAnalysisMarkers('bookmark', [{ id: 'analysis-1', kind: 'bookmark', startFrame: 10 }]);
    const ids = store.get('bookmark').map((m2) => m2.id);
    expect(ids).toContain('user-1');
    expect(ids).toContain('analysis-1');

    store.replaceAnalysisMarkers('bookmark', [{ id: 'analysis-2', kind: 'bookmark', startFrame: 20 }]);
    const idsAfter = store.get('bookmark').map((m2) => m2.id);
    expect(idsAfter).toEqual(['user-1', 'analysis-2']);
  });

  it('add keeps the per-kind array sorted', () => {
    const store = new MarkerStore();
    store.add(m('c', 30));
    store.add(m('a', 10));
    store.add(m('b', 20));
    expect(store.get('clipping').map((x) => x.startFrame)).toEqual([10, 20, 30]);
  });

  it('update re-sorts if startFrame moves', () => {
    const store = new MarkerStore();
    store.add(m('a', 10));
    store.add(m('b', 20));
    store.update('a', 'clipping', { startFrame: 30 });
    expect(store.get('clipping').map((x) => x.id)).toEqual(['b', 'a']);
  });
});
