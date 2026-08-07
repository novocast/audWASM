// Reads colours from CSS custom properties (M17 "Theming and accessibility") so light/dark/
// high-contrast themes work without the renderer knowing which theme is active. WebGL reads the
// same computed values and uploads them as uniforms rather than hardcoding a palette in a shader.

export interface RgbaColor {
  r: number;
  g: number;
  b: number;
  a: number;
}

export interface ThemeTokens {
  background: RgbaColor;
  grid: RgbaColor;
  rulerText: RgbaColor;
  rulerTick: RgbaColor;
  waveformFill: RgbaColor;
  waveformRms: RgbaColor;
  waveformRawPcm: RgbaColor;
  selectionFill: RgbaColor;
  selectionStroke: RgbaColor;
  selectionHandle: RgbaColor;
  cursor: RgbaColor;
  clipping: RgbaColor;
  focusRing: RgbaColor;
}

/** Custom-property name for each token. All must resolve (via `var(--x, fallback)` in the
 *  stylesheet or an explicit fallback here) so a missing theme never renders blank/invisible UI. */
const kTokenVarNames: Record<keyof ThemeTokens, string> = {
  background: '--aud-color-background',
  grid: '--aud-color-grid',
  rulerText: '--aud-color-ruler-text',
  rulerTick: '--aud-color-ruler-tick',
  waveformFill: '--aud-color-waveform',
  waveformRms: '--aud-color-waveform-rms',
  waveformRawPcm: '--aud-color-waveform-raw-pcm',
  selectionFill: '--aud-color-selection-fill',
  selectionStroke: '--aud-color-selection-stroke',
  selectionHandle: '--aud-color-selection-handle',
  cursor: '--aud-color-cursor',
  clipping: '--aud-color-clipping',
  focusRing: '--aud-color-focus-ring',
};

/** Used when a custom property is unset (e.g. no stylesheet loaded yet, or in a unit test with no
 *  DOM styling) — a usable dark theme, not blank. */
const kFallbackTokens: ThemeTokens = {
  background: { r: 0.09, g: 0.09, b: 0.11, a: 1 },
  grid: { r: 1, g: 1, b: 1, a: 0.06 },
  rulerText: { r: 0.75, g: 0.76, b: 0.8, a: 1 },
  rulerTick: { r: 1, g: 1, b: 1, a: 0.2 },
  waveformFill: { r: 0.37, g: 0.83, b: 1, a: 1 },
  waveformRms: { r: 0.37, g: 0.83, b: 1, a: 0.55 },
  waveformRawPcm: { r: 1, g: 0.72, b: 0.37, a: 1 },
  selectionFill: { r: 0.37, g: 0.6, b: 1, a: 0.22 },
  selectionStroke: { r: 0.37, g: 0.6, b: 1, a: 0.8 },
  selectionHandle: { r: 0.9, g: 0.93, b: 1, a: 1 },
  cursor: { r: 1, g: 1, b: 1, a: 0.95 },
  clipping: { r: 1, g: 0.25, b: 0.25, a: 1 },
  focusRing: { r: 1, g: 0.8, b: 0.2, a: 1 },
};

function parseColor(raw: string): RgbaColor | null {
  const s = raw.trim();
  if (s.length === 0) return null;

  const hexMatch = /^#([0-9a-f]{3}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(s);
  if (hexMatch) {
    const hex = hexMatch[1]!;
    const expand = (h: string): string => (h.length === 1 ? h + h : h);
    if (hex.length === 3) {
      const [r, g, b] = hex.split('');
      return { r: parseInt(expand(r!), 16) / 255, g: parseInt(expand(g!), 16) / 255, b: parseInt(expand(b!), 16) / 255, a: 1 };
    }
    const r = parseInt(hex.slice(0, 2), 16) / 255;
    const g = parseInt(hex.slice(2, 4), 16) / 255;
    const b = parseInt(hex.slice(4, 6), 16) / 255;
    const a = hex.length === 8 ? parseInt(hex.slice(6, 8), 16) / 255 : 1;
    return { r, g, b, a };
  }

  const funcMatch = /^rgba?\(([^)]+)\)$/i.exec(s);
  if (funcMatch) {
    const parts = funcMatch[1]!.split(/[\s,/]+/).filter((p) => p.length > 0);
    const toUnit = (v: string): number => (v.endsWith('%') ? parseFloat(v) / 100 : parseFloat(v) / 255);
    const r = toUnit(parts[0] ?? '0');
    const g = toUnit(parts[1] ?? '0');
    const b = toUnit(parts[2] ?? '0');
    const aPart = parts[3];
    const a = aPart === undefined ? 1 : aPart.endsWith('%') ? parseFloat(aPart) / 100 : parseFloat(aPart);
    return { r, g, b, a };
  }

  return null;
}

/** Reads all theme tokens from `el`'s computed style. Call once per theme-change event (a
 *  `prefers-color-scheme`/`prefers-contrast` media query listener, or a manual theme toggle) —
 *  not every frame; computed-style reads are comparatively expensive. */
export function readThemeTokens(el: Element): ThemeTokens {
  const computed = getComputedStyle(el);
  const tokens = {} as ThemeTokens;
  for (const key of Object.keys(kTokenVarNames) as (keyof ThemeTokens)[]) {
    const raw = computed.getPropertyValue(kTokenVarNames[key]);
    tokens[key] = parseColor(raw) ?? kFallbackTokens[key];
  }
  return tokens;
}

/** Flat RGBA float array in token order, for a single `uniform4fv` upload — cheaper than one
 *  `uniform4f` call per colour, and keeps the shader's uniform layout in one place. */
export function themeTokensToFloatArray(theme: ThemeTokens): Float32Array {
  const keys = Object.keys(kTokenVarNames) as (keyof ThemeTokens)[];
  const out = new Float32Array(keys.length * 4);
  keys.forEach((key, i) => {
    const c = theme[key];
    out[i * 4 + 0] = c.r;
    out[i * 4 + 1] = c.g;
    out[i * 4 + 2] = c.b;
    out[i * 4 + 3] = c.a;
  });
  return out;
}

export function rgbaToCssString(c: RgbaColor): string {
  return `rgba(${Math.round(c.r * 255)}, ${Math.round(c.g * 255)}, ${Math.round(c.b * 255)}, ${c.a})`;
}

/** Subscribes to the media queries that should trigger a theme re-read (M17 "Theming"). Returns
 *  an unsubscribe function. Does not itself call `readThemeTokens` — the caller decides when. */
export function watchThemeChanges(onChange: () => void): () => void {
  const queries = [
    matchMedia('(prefers-color-scheme: dark)'),
    matchMedia('(prefers-contrast: more)'),
    matchMedia('(prefers-contrast: high)'),
  ];
  for (const q of queries) q.addEventListener('change', onChange);
  return () => {
    for (const q of queries) q.removeEventListener('change', onChange);
  };
}
