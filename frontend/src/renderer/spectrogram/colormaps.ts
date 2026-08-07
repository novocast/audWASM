// Colour maps as data, not code (M07 "Colour maps" decision), generated once into 256x1 RGBA LUT
// textures the fragment shader samples. Viridis/Magma/Inferno are perceptually uniform and
// colour-blind safe — the responsible defaults; Greyscale is for print/bias-free judging; Classic
// is the familiar (but explicitly *not* perceptually uniform — labelled as such in the UI) blue/
// cyan/yellow/red ramp some other audio tools default to.
//
// Perceptual uniformity is not aesthetics here: a non-uniform map creates apparent edges where
// there is no change in the underlying data, which in an analysis tool means seeing structure that
// isn't there (M07 design doc).
//
// Each map is stored as a compact set of anchor stops (representative published colour-map control
// points, not the full 256-entry table) and linearly interpolated to 256 entries at load — the same
// technique most non-matplotlib implementations of these maps use, and small enough to read at a
// glance rather than a wall of 256 opaque hex literals.

export type SpectrogramColorMapName = 'viridis' | 'magma' | 'inferno' | 'greyscale' | 'classic';

type Stop = readonly [number, number, number]; // 0-255 RGB

const kViridisStops: readonly Stop[] = [
  [68, 1, 84],
  [72, 26, 108],
  [71, 47, 125],
  [65, 68, 135],
  [57, 86, 140],
  [49, 104, 142],
  [42, 120, 142],
  [35, 136, 142],
  [31, 152, 139],
  [34, 168, 132],
  [53, 183, 121],
  [84, 197, 104],
  [122, 209, 81],
  [165, 219, 54],
  [210, 226, 27],
  [253, 231, 37],
];

const kMagmaStops: readonly Stop[] = [
  [0, 0, 4],
  [11, 9, 36],
  [32, 17, 75],
  [59, 15, 112],
  [87, 21, 126],
  [114, 31, 129],
  [140, 41, 129],
  [168, 50, 125],
  [196, 60, 117],
  [222, 73, 104],
  [241, 96, 93],
  [250, 127, 94],
  [254, 159, 109],
  [254, 191, 132],
  [253, 222, 160],
  [252, 253, 191],
];

const kInfernoStops: readonly Stop[] = [
  [0, 0, 4],
  [10, 7, 35],
  [31, 12, 72],
  [58, 9, 99],
  [84, 19, 111],
  [110, 27, 109],
  [136, 34, 106],
  [161, 42, 99],
  [187, 50, 88],
  [211, 61, 72],
  [230, 78, 52],
  [244, 100, 27],
  [252, 128, 5],
  [252, 161, 8],
  [246, 196, 54],
  [252, 255, 164],
];

const kClassicStops: readonly Stop[] = [
  [0, 0, 80],
  [0, 0, 255],
  [0, 200, 255],
  [0, 255, 200],
  [255, 255, 0],
  [255, 128, 0],
  [255, 0, 0],
];

function greyscaleStops(): readonly Stop[] {
  return [
    [0, 0, 0],
    [255, 255, 255],
  ];
}

function stopsFor(name: SpectrogramColorMapName): readonly Stop[] {
  switch (name) {
    case 'viridis':
      return kViridisStops;
    case 'magma':
      return kMagmaStops;
    case 'inferno':
      return kInfernoStops;
    case 'greyscale':
      return greyscaleStops();
    case 'classic':
      return kClassicStops;
  }
}

/** Builds a 256-entry RGBA LUT (Uint8Array, length 1024) by linearly interpolating `name`'s anchor
 *  stops. Alpha is always 255 — the colour map never encodes transparency. */
export function buildColorMapLut(name: SpectrogramColorMapName): Uint8Array {
  const stops = stopsFor(name);
  const lut = new Uint8Array(256 * 4);
  const lastIndex = stops.length - 1;

  for (let i = 0; i < 256; i++) {
    const t = (i / 255) * lastIndex;
    const lo = Math.min(Math.floor(t), lastIndex - 1);
    const hi = lo + 1;
    const frac = t - lo;

    const [r0, g0, b0] = stops[lo]!;
    const [r1, g1, b1] = stops[hi]!;

    lut[i * 4 + 0] = Math.round(r0 + (r1 - r0) * frac);
    lut[i * 4 + 1] = Math.round(g0 + (g1 - g0) * frac);
    lut[i * 4 + 2] = Math.round(b0 + (b1 - b0) * frac);
    lut[i * 4 + 3] = 255;
  }

  return lut;
}

export const kColorMapNames: readonly SpectrogramColorMapName[] = [
  'viridis',
  'magma',
  'inferno',
  'greyscale',
  'classic',
];

/** True for maps documented as *not* perceptually uniform — the UI should label these explicitly
 *  (M07: "not perceptually uniform, labelled as such"). */
export function isPerceptuallyUniform(name: SpectrogramColorMapName): boolean {
  return name !== 'classic';
}
