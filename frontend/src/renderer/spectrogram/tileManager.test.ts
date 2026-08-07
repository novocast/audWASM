import { describe, expect, it } from 'vitest';
import {
  chooseFftSize,
  chooseLevel,
  computeVisibleTiles,
  foldFactorForLevel,
  hopForLevel,
  kFftSizeSteps,
  samplesPerColumn,
} from './tileManager.ts';
import { defaultViewState } from '../viewState.ts';

describe('hopForLevel / foldFactorForLevel', () => {
  it('matches engine/spectrogram/tile_generator.hpp: fftSize/4, fftSize/2, fftSize, then fftSize again', () => {
    const fftSize = 4096;
    expect(hopForLevel(fftSize, 0)).toBe(1024);
    expect(hopForLevel(fftSize, 1)).toBe(2048);
    expect(hopForLevel(fftSize, 2)).toBe(4096);
    expect(hopForLevel(fftSize, 3)).toBe(4096);
    expect(hopForLevel(fftSize, 5)).toBe(4096);
  });

  it('fold factor is 1 for levels 0-2, then doubles starting at level 3', () => {
    expect(foldFactorForLevel(0)).toBe(1);
    expect(foldFactorForLevel(2)).toBe(1);
    expect(foldFactorForLevel(3)).toBe(2);
    expect(foldFactorForLevel(4)).toBe(4);
    expect(foldFactorForLevel(5)).toBe(8);
  });

  it('samplesPerColumn is monotonically non-decreasing with level', () => {
    const fftSize = 4096;
    let previous = 0;
    for (let level = 0; level <= 8; level++) {
      const current = samplesPerColumn(fftSize, level);
      expect(current).toBeGreaterThanOrEqual(previous);
      previous = current;
    }
  });
});

describe('chooseLevel', () => {
  it('picks a coarser level as framesPerPixel grows', () => {
    const fftSize = 4096;
    const levelAtFine = chooseLevel(fftSize, 10);
    const levelAtCoarse = chooseLevel(fftSize, 100000);
    expect(levelAtCoarse).toBeGreaterThan(levelAtFine);
  });
});

describe('chooseFftSize (adaptive fftSize with hysteresis, M07 acceptance criteria)', () => {
  it('starts at the finest step for a heavily zoomed-in view', () => {
    expect(chooseFftSize(1, 8192)).toBe(kFftSizeSteps[0]);
  });

  it('steps up only across the defined coarse steps, never continuously', () => {
    let fftSize: number = kFftSizeSteps[0];
    const seen = new Set<number>();
    for (let fpp = 1; fpp <= 5000; fpp *= 1.2) {
      fftSize = chooseFftSize(fpp, fftSize);
      seen.add(fftSize);
    }
    for (const size of seen) {
      expect(kFftSizeSteps as readonly number[]).toContain(size);
    }
  });

  it('a scripted continuous zoom in/out near a boundary does not thrash (hysteresis holds)', () => {
    // Oscillate framesPerPixel right around the 2048<->8192 boundary (80) — without hysteresis
    // this would flip fftSize on almost every step.
    let fftSize: number = kFftSizeSteps[0];
    let switches = 0;
    let previous = fftSize;
    for (let i = 0; i < 40; i++) {
      const fpp = 80 + (i % 2 === 0 ? 5 : -5); // tiny oscillation around the boundary
      fftSize = chooseFftSize(fpp, fftSize);
      if (fftSize !== previous) switches++;
      previous = fftSize;
    }
    expect(switches).toBe(0);
  });

  it('does switch once the zoom moves clearly past a boundary, and hysteresis prevents an immediate switch back', () => {
    let fftSize: number = kFftSizeSteps[0];
    fftSize = chooseFftSize(80 * 1.5, fftSize); // clearly past the boundary, with margin
    expect(fftSize).toBe(kFftSizeSteps[1]);
    // A small nudge back toward (but not past) the boundary must not immediately revert.
    fftSize = chooseFftSize(80, fftSize);
    expect(fftSize).toBe(kFftSizeSteps[1]);
  });
});

describe('computeVisibleTiles', () => {
  it('returns tiles for every channel, priority-ordered by distance from the viewport centre', () => {
    const view = {
      ...defaultViewState(),
      startFrame: 100_000,
      framesPerPixel: 4,
      widthCss: 800,
      devicePixelRatio: 1,
    };
    const refs = computeVisibleTiles(view, 4096, 2);
    expect(refs.length).toBeGreaterThan(0);
    // Every ref must be for channel 0 or 1.
    for (const ref of refs) expect([0, 1]).toContain(ref.channel);
    // Priority order: consecutive refs' distance-from-centre must be non-decreasing within a channel.
    const centreFrame = view.startFrame + (view.framesPerPixel * view.widthCss) / 2;
    const span = hopForLevel(4096, refs[0]!.level) * foldFactorForLevel(refs[0]!.level) * 256;
    let previousDistance = -1;
    for (const ref of refs.filter((r) => r.channel === 0)) {
      const centre = ref.tileX * span + span / 2;
      const distance = Math.abs(centre - centreFrame);
      expect(distance).toBeGreaterThanOrEqual(previousDistance - 1e-6);
      previousDistance = distance;
    }
  });

  it('returns nothing for zero channels or zero width', () => {
    const view = { ...defaultViewState(), widthCss: 800 };
    expect(computeVisibleTiles(view, 4096, 0)).toEqual([]);
    expect(computeVisibleTiles({ ...view, widthCss: 0 }, 4096, 1)).toEqual([]);
  });
});
