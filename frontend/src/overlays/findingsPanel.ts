// M18's findings list panel: "all markers, filterable and sortable, click to navigate. This is the
// primary interface for a QA workflow — most users will work from the list, not the canvas."
// Plain DOM construction, matching metadataPanel.ts's established convention (no framework in this
// repo).

import type { Marker, OverlayKind } from './model.ts';
import { metaFor } from './model.ts';
import type { MarkerStore } from './store.ts';

export type FindingsSortKey = 'time' | 'kind' | 'severity';

export interface FindingsPanelHost {
  onSelect(marker: Marker): void;
  /** Formats a marker's start (and end, if any) for display — the host owns sample-rate/units
   *  conversion so this module has no engine dependency. */
  formatTime(frame: number): string;
}

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  opts?: { text?: string; className?: string },
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (opts?.text !== undefined) node.textContent = opts.text;
  if (opts?.className) node.className = opts.className;
  return node;
}

const kSeverityRank: Record<string, number> = { error: 3, warning: 2, info: 1 };

/** A real analyser can legitimately return tens of thousands of findings (e.g. a defect-heavy
 *  transient pass) — M18's 50 000-marker performance budget is about the *canvas* draw path
 *  (density-managed), not this list, which builds one plain DOM row per marker with no
 *  virtualisation. Rendering all of them makes the page itself sluggish enough to stall the
 *  audio pump loop's setInterval and crackle playback, independent of anything drawn. Capping
 *  here is the minimal fix; a virtualised list is the real one, tracked as a follow-up. */
const kMaxRenderedRows = 500;

export class FindingsPanel {
  readonly root: HTMLElement;
  private readonly listEl: HTMLElement;
  private readonly kindFilterEl: HTMLSelectElement;
  private sortKey: FindingsSortKey = 'time';
  private kindFilter: OverlayKind | 'all' = 'all';
  private selectedId: string | null = null;

  constructor(private readonly store: MarkerStore, private readonly host: FindingsPanelHost) {
    this.root = el('div', { className: 'aud-findings-panel' });

    const toolbar = el('div', { className: 'aud-findings-toolbar' });
    this.kindFilterEl = document.createElement('select');
    this.kindFilterEl.className = 'aud-findings-kind-filter';
    this.kindFilterEl.addEventListener('change', () => {
      this.kindFilter = this.kindFilterEl.value as OverlayKind | 'all';
      this.render();
    });
    toolbar.appendChild(this.kindFilterEl);

    for (const key of ['time', 'kind', 'severity'] as const) {
      const btn = el('button', { text: `Sort: ${key}`, className: 'aud-findings-sort-btn' });
      btn.addEventListener('click', () => {
        this.sortKey = key;
        this.render();
      });
      toolbar.appendChild(btn);
    }
    this.root.appendChild(toolbar);

    this.listEl = el('div', { className: 'aud-findings-list' });
    this.root.appendChild(this.listEl);

    this.render();
  }

  /** Call after the store's contents change (new analysis results, a bookmark added/removed). */
  refresh(): void {
    this.render();
  }

  setSelected(id: string | null): void {
    this.selectedId = id;
    this.render();
  }

  private render(): void {
    const kinds = new Set(this.store.kinds());
    this.kindFilterEl.replaceChildren(
      el('option', { text: `All kinds (${this.store.all().length})` }),
      ...[...kinds].sort().map((k) => {
        const opt = el('option', { text: `${k} (${this.store.get(k).length})` });
        opt.value = k;
        return opt;
      }),
    );
    this.kindFilterEl.value = this.kindFilter;

    let markers = this.store.all();
    if (this.kindFilter !== 'all') markers = markers.filter((m) => m.kind === this.kindFilter);
    markers = [...markers].sort((a, b) => this.compare(a, b));

    const total = markers.length;
    const capped = total > kMaxRenderedRows;
    const toRender = capped ? markers.slice(0, kMaxRenderedRows) : markers;

    this.listEl.replaceChildren(
      ...toRender.map((m) => {
        const row = el('div', { className: 'aud-findings-row' });
        row.classList.toggle('aud-findings-row--selected', m.id === this.selectedId);
        row.classList.toggle(`aud-findings-row--${m.severity ?? 'default'}`, true);
        row.appendChild(el('span', { className: 'aud-findings-kind', text: m.kind }));
        row.appendChild(el('span', { className: 'aud-findings-time', text: this.host.formatTime(m.startFrame) }));
        row.appendChild(el('span', { className: 'aud-findings-label', text: m.label ?? '' }));
        row.tabIndex = 0;
        row.addEventListener('click', () => {
          this.selectedId = m.id;
          this.host.onSelect(m);
          this.render();
        });
        row.addEventListener('keydown', (e) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            row.click();
          }
        });
        return row;
      }),
    );

    if (total === 0) {
      this.listEl.appendChild(el('div', { className: 'aud-findings-empty', text: 'No findings.' }));
    } else if (capped) {
      this.listEl.appendChild(
        el('div', {
          className: 'aud-findings-capped',
          text: `Showing ${kMaxRenderedRows} of ${total} — narrow the kind filter to see the rest.`,
        }),
      );
    }
  }

  private compare(a: Marker, b: Marker): number {
    switch (this.sortKey) {
      case 'time':
        return a.startFrame - b.startFrame;
      case 'kind':
        return a.kind.localeCompare(b.kind) || a.startFrame - b.startFrame;
      case 'severity':
        return (kSeverityRank[b.severity ?? ''] ?? 0) - (kSeverityRank[a.severity ?? ''] ?? 0) || a.startFrame - b.startFrame;
    }
  }
}

/** Groups markers by kind, sorted by each kind's registry priority (highest first) — used when a
 *  caller wants a "group by kind" view rather than the flat sortable list above. */
export function groupFindingsByKind(store: MarkerStore): Array<{ kind: OverlayKind; markers: readonly Marker[] }> {
  return store
    .kinds()
    .map((kind) => ({ kind, markers: store.get(kind) }))
    .sort((a, b) => metaFor(b.kind).priority - metaFor(a.kind).priority);
}
