// Backend capability detection and selection, with a manual override for debugging (M17 tasks
// list). WebGL2 is the default where available; Canvas2D is the fallback and remains fully
// capable of drawing everything except the spectrogram (M07).

import { detectWebgl2Support } from './webgl/glUtil.ts';
import type { BackendCapability, Renderer } from './renderer.ts';
import { Canvas2DRenderer } from './canvas2d/canvas2dRenderer.ts';
import { WebglRenderer } from './webgl/webglRenderer.ts';

export type BackendName = 'canvas2d' | 'webgl2';
export type BackendOverride = BackendName | 'auto';

const kOverrideQueryParam = 'audRenderer';
const kOverrideStorageKey = 'aud.renderer.override';

export function detectBackendCapabilities(): BackendCapability[] {
  const webgl2Available = detectWebgl2Support();
  return [
    { backend: 'canvas2d', available: true },
    {
      backend: 'webgl2',
      available: webgl2Available,
      ...(webgl2Available ? {} : { reason: 'webgl2 context unavailable' }),
    },
  ];
}

/** Reads a manual override from `?audRenderer=canvas2d|webgl2` (checked first, so a shared link
 *  reproduces a bug) or `localStorage['aud.renderer.override']` (checked second, for a sticky
 *  local toggle). Falls back to 'auto'. */
export function readBackendOverride(location: Location = window.location): BackendOverride {
  const param = new URLSearchParams(location.search).get(kOverrideQueryParam);
  if (param === 'canvas2d' || param === 'webgl2') return param;
  try {
    const stored = localStorage.getItem(kOverrideStorageKey);
    if (stored === 'canvas2d' || stored === 'webgl2') return stored;
  } catch {
    // localStorage can throw in restricted contexts (private browsing quotas, etc.) — 'auto' is
    // a perfectly good fallback, not a failure worth surfacing.
  }
  return 'auto';
}

export function setBackendOverride(override: BackendOverride): void {
  try {
    if (override === 'auto') localStorage.removeItem(kOverrideStorageKey);
    else localStorage.setItem(kOverrideStorageKey, override);
  } catch {
    // Same rationale as above — best-effort persistence, never a hard requirement.
  }
}

export function selectBackendName(override: BackendOverride, capabilities: BackendCapability[]): BackendName {
  const webgl2 = capabilities.find((c) => c.backend === 'webgl2');
  if (override !== 'auto') {
    const requested = capabilities.find((c) => c.backend === override);
    if (requested?.available) return override;
  }
  return webgl2?.available ? 'webgl2' : 'canvas2d';
}

export function createRenderer(backend: BackendName): Renderer {
  return backend === 'webgl2' ? new WebglRenderer() : new Canvas2DRenderer();
}
