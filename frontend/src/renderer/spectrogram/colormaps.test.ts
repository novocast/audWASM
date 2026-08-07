import { describe, expect, it } from 'vitest';
import { buildColorMapLut, isPerceptuallyUniform, kColorMapNames } from './colormaps.ts';

describe('buildColorMapLut', () => {
  it('builds a 256-entry RGBA LUT (1024 bytes) for every colour map', () => {
    for (const name of kColorMapNames) {
      const lut = buildColorMapLut(name);
      expect(lut.length).toBe(256 * 4);
    }
  });

  it('alpha is always fully opaque', () => {
    for (const name of kColorMapNames) {
      const lut = buildColorMapLut(name);
      for (let i = 0; i < 256; i++) {
        expect(lut[i * 4 + 3]).toBe(255);
      }
    }
  });

  it('every RGB channel stays in [0,255]', () => {
    for (const name of kColorMapNames) {
      const lut = buildColorMapLut(name);
      for (let i = 0; i < lut.length; i++) {
        expect(lut[i]).toBeGreaterThanOrEqual(0);
        expect(lut[i]).toBeLessThanOrEqual(255);
      }
    }
  });

  it('is deterministic', () => {
    for (const name of kColorMapNames) {
      expect(buildColorMapLut(name)).toEqual(buildColorMapLut(name));
    }
  });
});

describe('isPerceptuallyUniform', () => {
  it('flags only classic as not perceptually uniform', () => {
    expect(isPerceptuallyUniform('classic')).toBe(false);
    for (const name of kColorMapNames.filter((n) => n !== 'classic')) {
      expect(isPerceptuallyUniform(name)).toBe(true);
    }
  });
});
