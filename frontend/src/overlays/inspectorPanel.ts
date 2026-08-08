// M18's marker inspector: "Clicking a marker opens an inspector showing everything the analyser
// knows about it ... This is what makes the analysis auditable rather than magic." Plus "zoom to
// this" and "loop this" actions reusing M03/M14's audition. Plain DOM, matching metadataPanel.ts.

import type { Marker } from './model.ts';

export interface InspectorPanelHost {
  formatTime(frame: number): string;
  onZoomTo(marker: Marker): void;
  onLoopThis(marker: Marker): void;
  /** Present only for `userCreated` markers (bookmarks) — omit to hide edit/delete affordances. */
  onEditLabel?(marker: Marker, newLabel: string): void;
  onDelete?(marker: Marker): void;
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

export class InspectorPanel {
  readonly root: HTMLElement;

  constructor(private readonly host: InspectorPanelHost) {
    this.root = el('div', { className: 'aud-inspector-panel' });
    this.showEmpty();
  }

  showEmpty(): void {
    this.root.replaceChildren(el('div', { className: 'aud-inspector-empty', text: 'No marker selected.' }));
  }

  show(marker: Marker): void {
    const rows: HTMLElement[] = [];
    const addRow = (label: string, value: string): void => {
      const row = el('div', { className: 'aud-inspector-row' });
      row.appendChild(el('span', { className: 'aud-inspector-label', text: label }));
      row.appendChild(el('span', { className: 'aud-inspector-value', text: value }));
      rows.push(row);
    };

    addRow('Kind', marker.kind);
    addRow('Start', this.host.formatTime(marker.startFrame));
    if (marker.endFrame !== undefined) addRow('End', this.host.formatTime(marker.endFrame));
    if (marker.channel !== undefined) addRow('Channel', String(marker.channel));
    if (marker.severity) addRow('Severity', marker.severity);
    if (marker.label) addRow('Label', marker.label);
    addRow('User-created', marker.userCreated ? 'yes' : 'no');

    const dataBlock =
      marker.data !== undefined
        ? el('pre', { className: 'aud-inspector-data', text: JSON.stringify(marker.data, null, 2) })
        : null;

    const actions = el('div', { className: 'aud-inspector-actions' });
    const zoomBtn = el('button', { text: 'Zoom to this' });
    zoomBtn.addEventListener('click', () => this.host.onZoomTo(marker));
    const loopBtn = el('button', { text: 'Loop this' });
    loopBtn.addEventListener('click', () => this.host.onLoopThis(marker));
    actions.append(zoomBtn, loopBtn);

    if (marker.userCreated && this.host.onEditLabel) {
      const editBtn = el('button', { text: 'Edit label' });
      editBtn.addEventListener('click', () => {
        const next = window.prompt('Bookmark label', marker.label ?? '');
        if (next !== null) this.host.onEditLabel?.(marker, next);
      });
      actions.appendChild(editBtn);
    }
    if (marker.userCreated && this.host.onDelete) {
      const deleteBtn = el('button', { text: 'Delete' });
      deleteBtn.addEventListener('click', () => this.host.onDelete?.(marker));
      actions.appendChild(deleteBtn);
    }

    this.root.replaceChildren(
      el('h3', { className: 'aud-inspector-title', text: `Marker: ${marker.id}` }),
      ...rows,
      ...(dataBlock ? [dataBlock] : []),
      actions,
    );
  }
}
