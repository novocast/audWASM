import { describe, expect, it } from 'vitest';
import { hitTestMarkers, type HitCandidate } from './hitTest.ts';
import type { Marker } from './model.ts';

function marker(id: string, kind: Marker['kind']): Marker {
  return { id, kind, startFrame: 0 };
}

function candidate(id: string, kind: Marker['kind'], px: number, laneOrder = 0): HitCandidate {
  return { marker: marker(id, kind), pixelStart: px, pixelEnd: px, laneOrder };
}

describe('hitTestMarkers', () => {
  it('returns null when nothing is within tolerance', () => {
    const candidates = [candidate('a', 'beat', 100)];
    expect(hitTestMarkers(candidates, 0, 5)).toBeNull();
  });

  it('picks the topmost lane over a closer candidate in a lower lane', () => {
    const candidates = [
      candidate('lower-but-closer', 'beat', 10, /* laneOrder */ 1),
      candidate('topmost', 'beat', 9, /* laneOrder */ 0),
    ];
    const hit = hitTestMarkers(candidates, 10, 5);
    expect(hit?.id).toBe('topmost');
  });

  it('within the same lane, picks the nearest candidate', () => {
    const candidates = [candidate('far', 'beat', 2, 0), candidate('near', 'beat', 8, 0)];
    const hit = hitTestMarkers(candidates, 10, 10);
    expect(hit?.id).toBe('near');
  });

  it('breaks a same-lane, same-distance tie by kind priority (errors > defects > user > analysis)', () => {
    const candidates = [candidate('analysis', 'beat', 10, 0), candidate('error', 'error', 10, 0)];
    const hit = hitTestMarkers(candidates, 10, 5);
    expect(hit?.id).toBe('error');
  });

  it('user-created bookmarks outrank plain analysis markers at equal distance', () => {
    const candidates = [candidate('analysis', 'beat', 10, 0), candidate('bookmark', 'bookmark', 10, 0)];
    const hit = hitTestMarkers(candidates, 10, 5);
    expect(hit?.id).toBe('bookmark');
  });

  it('defects outrank user bookmarks at equal distance', () => {
    const candidates = [candidate('bookmark', 'bookmark', 10, 0), candidate('defect', 'defect', 10, 0)];
    const hit = hitTestMarkers(candidates, 10, 5);
    expect(hit?.id).toBe('defect');
  });

  it('a region span counts as a hit anywhere inside it, with distance 0', () => {
    const candidates: HitCandidate[] = [{ marker: marker('region', 'silence'), pixelStart: 10, pixelEnd: 50, laneOrder: 0 }];
    expect(hitTestMarkers(candidates, 30, 0)?.id).toBe('region');
  });
});
