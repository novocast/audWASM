// The waveform canvas's accessible text alternative (M17 "Theming and accessibility": "needs an
// accessible text alternative ... rather than being an opaque black box to a screen reader").
// Renders into a visually-hidden live region alongside the canvas; the canvas itself should carry
// `role="img"` and `aria-describedby` pointing at this element's id.

export interface AccessibleSummaryInput {
  durationSeconds: number;
  sampleRate: number;
  channelCount: number;
  /** Peak absolute sample value across the whole track, 0..1 linear. Undefined while unknown. */
  peakLinear?: number;
  /** Integrated loudness in LUFS (M08), once available. Undefined until M08 lands/runs. */
  integratedLufs?: number;
  visibleStartSeconds: number;
  visibleEndSeconds: number;
}

function formatDuration(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  const parts = h > 0 ? [h, m, s] : [m, s];
  return parts.map((p, i) => (i === 0 ? String(p) : String(p).padStart(2, '0'))).join(':');
}

function formatDb(linear: number): string {
  if (linear <= 0) return '-inf dB';
  return `${(20 * Math.log10(linear)).toFixed(1)} dB`;
}

/** Builds the plain-text summary — duration, peak, loudness, and what's currently visible — a
 *  screen reader user gets in place of seeing the waveform shape. Cheap to call whenever the
 *  underlying facts change (not every rAF frame); the loop should throttle calls to view changes
 *  that matter (e.g. only when the visible range's rounded-to-second boundaries change). */
export function buildAccessibleSummary(input: AccessibleSummaryInput): string {
  const channelWord = input.channelCount === 1 ? 'mono' : input.channelCount === 2 ? 'stereo' : `${input.channelCount}-channel`;
  const parts = [
    `Audio waveform, ${formatDuration(input.durationSeconds)} duration, ${channelWord}, ${input.sampleRate} Hz.`,
  ];
  if (input.peakLinear !== undefined) {
    parts.push(`Peak level ${formatDb(input.peakLinear)}.`);
  }
  if (input.integratedLufs !== undefined) {
    parts.push(`Integrated loudness ${input.integratedLufs.toFixed(1)} LUFS.`);
  }
  parts.push(`Currently showing ${formatDuration(input.visibleStartSeconds)} to ${formatDuration(input.visibleEndSeconds)}.`);
  return parts.join(' ');
}

/** Creates (or reuses) a visually-hidden live-region element inside `container`, wires it to
 *  `canvas` via aria-describedby/role, and returns a setter for its text. */
export function attachAccessibleSummary(container: HTMLElement, canvas: HTMLElement): (text: string) => void {
  const existing = container.querySelector<HTMLElement>('[data-aud-accessible-summary]');
  const el = existing ?? document.createElement('div');
  if (!existing) {
    el.setAttribute('data-aud-accessible-summary', '');
    el.setAttribute('aria-live', 'polite');
    // Visually hidden but still reachable by assistive tech, unlike `display: none`/`hidden`.
    el.style.position = 'absolute';
    el.style.width = '1px';
    el.style.height = '1px';
    el.style.overflow = 'hidden';
    el.style.clipPath = 'inset(50%)';
    el.style.whiteSpace = 'nowrap';
    container.appendChild(el);
  }
  if (!el.id) el.id = `aud-summary-${Math.random().toString(36).slice(2)}`;
  canvas.setAttribute('role', 'img');
  canvas.setAttribute('aria-describedby', el.id);

  return (text: string): void => {
    el.textContent = text;
  };
}
