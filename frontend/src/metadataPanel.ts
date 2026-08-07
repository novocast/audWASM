// M15's metadata viewer — the first analysis-result UI panel in the frontend (no prior panel to
// model conventions after, per the milestone doc's UI task list: "metadata panel (mapped fields),
// raw tag table (everything), cover art gallery, lyrics view, chapter list; all fields copyable").
// Plain DOM construction, matching main.ts's style — no framework in this repo yet.

import type {
  CuePointResult,
  LyricsResult,
  MetadataResult,
  PictureInfo,
} from '../../bindings/wasm/aud_wasm.d.ts';

export interface MetadataPanelHost {
  /** Materialises picture `index`'s bytes — see engine.ts's Metadata.getPictureBytes(). */
  getPictureBytes(index: number): Uint8Array;
}

const kMappedFields: Array<[key: keyof MetadataResult, label: string]> = [
  ['title', 'Title'],
  ['artist', 'Artist'],
  ['albumArtist', 'Album artist'],
  ['album', 'Album'],
  ['genre', 'Genre'],
  ['composer', 'Composer'],
  ['trackNumber', 'Track'],
  ['trackTotal', 'Track total'],
  ['discNumber', 'Disc'],
  ['discTotal', 'Disc total'],
  ['year', 'Year'],
  ['date', 'Date'],
  ['bpm', 'BPM'],
  ['isrc', 'ISRC'],
  ['upc', 'UPC'],
  ['catalogNumber', 'Catalog #'],
  ['musicBrainzTrackId', 'MusicBrainz track ID'],
  ['musicBrainzAlbumId', 'MusicBrainz album ID'],
  ['publisher', 'Publisher'],
  ['copyright', 'Copyright'],
  ['encodedBy', 'Encoded by'],
  ['encoderSettings', 'Encoder settings'],
  ['comment', 'Comment'],
];

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  opts?: { text?: string; className?: string; attrs?: Record<string, string> },
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (opts?.text !== undefined) node.textContent = opts.text;
  if (opts?.className) node.className = opts.className;
  if (opts?.attrs) for (const [k, v] of Object.entries(opts.attrs)) node.setAttribute(k, v);
  return node;
}

/** Every field row gets one of these — M15's "all fields copyable". Falls back to a no-op if the
 *  Clipboard API isn't available (e.g. non-HTTPS/non-localhost origins) rather than throwing. */
function copyButton(text: string): HTMLButtonElement {
  const btn = el('button', { text: 'Copy', attrs: { type: 'button', title: 'Copy to clipboard' } });
  btn.addEventListener('click', () => {
    navigator.clipboard?.writeText(text).catch(() => {
      /* clipboard unavailable — the value is still visible and selectable */
    });
  });
  return btn;
}

function fieldRow(label: string, value: string): HTMLElement {
  const row = el('div', { className: 'metadata-field' });
  row.append(el('dt', { text: label }), el('dd', { text: value }), copyButton(value));
  return row;
}

function renderMappedFields(result: MetadataResult): HTMLElement {
  const dl = el('dl', { className: 'metadata-fields' });
  let anyField = false;
  for (const [key, label] of kMappedFields) {
    const value = result[key];
    if (value === null || value === undefined || value === '') continue;
    anyField = true;
    dl.append(fieldRow(label, String(value)));
  }
  if (!anyField) {
    dl.append(el('p', { text: 'No mapped tags found in this file.' }));
  }
  return dl;
}

function renderReplayGain(result: MetadataResult): HTMLElement | null {
  if (result.replayGain.sources.length === 0) return null;
  const section = el('div', { className: 'metadata-replaygain' });
  section.append(el('h3', { text: 'ReplayGain' }));
  const table = el('table');
  const head = el('tr');
  for (const label of ['Source', 'Track gain', 'Track peak', 'Album gain', 'Album peak']) {
    head.append(el('th', { text: label }));
  }
  table.append(head);
  for (const s of result.replayGain.sources) {
    const row = el('tr');
    row.append(
      el('td', { text: s.origin }),
      el('td', { text: s.trackGainDb !== null ? `${s.trackGainDb.toFixed(2)} dB` : '—' }),
      el('td', { text: s.trackPeak !== null ? s.trackPeak.toFixed(4) : '—' }),
      el('td', { text: s.albumGainDb !== null ? `${s.albumGainDb.toFixed(2)} dB` : '—' }),
      el('td', { text: s.albumPeak !== null ? s.albumPeak.toFixed(4) : '—' }),
    );
    table.append(row);
  }
  section.append(table);
  section.append(
    el('p', {
      className: 'metadata-note',
      text:
        'Not applied — read-only reporting only (M02). Cross-referencing against measured ' +
        'integrated loudness (M08) is wired up once that analyser has its own panel.',
    }),
  );
  return section;
}

function pictureTypeName(type: number): string {
  const names = [
    'Other', 'File icon', 'Other file icon', 'Front cover', 'Back cover', 'Leaflet page', 'Media',
    'Lead artist', 'Artist', 'Conductor', 'Band', 'Composer', 'Lyricist', 'Recording location',
    'During recording', 'During performance', 'Video screen capture', 'Fish', 'Illustration',
    'Band logo', 'Publisher logo',
  ];
  return names[type] ?? `Type ${type}`;
}

function renderPictures(host: MetadataPanelHost, pictures: PictureInfo[]): HTMLElement | null {
  if (pictures.length === 0) return null;
  const section = el('div', { className: 'metadata-pictures' });
  section.append(el('h3', { text: `Cover art (${pictures.length})` }));
  const gallery = el('div', { className: 'metadata-picture-gallery' });

  for (const pic of pictures) {
    const figure = el('figure', { className: 'metadata-picture' });
    const bytes = host.getPictureBytes(pic.index);
    const mime = pic.detectedMimeType || pic.declaredMimeType || 'application/octet-stream';

    if (bytes.length > 0) {
      // M15's decision: never decode image formats in C++ — hand the bytes to the browser's own
      // hardened decoder via a Blob URL rather than shipping a decoder in the WASM binary.
      const blob = new Blob([bytes as BlobPart], { type: mime });
      const url = URL.createObjectURL(blob);
      const img = el('img', { attrs: { src: url, alt: pic.description || pictureTypeName(pic.type), loading: 'lazy' } });
      img.style.maxWidth = '160px';
      img.style.maxHeight = '160px';
      figure.append(img);
    } else {
      figure.append(el('p', { text: '(no bytes)' }));
    }

    const caption = el('figcaption');
    caption.append(el('div', { text: pictureTypeName(pic.type) }));
    if (pic.description) caption.append(el('div', { text: pic.description }));
    caption.append(el('div', { className: 'metadata-note', text: `${mime}, ${pic.byteCount.toLocaleString()} bytes` }));
    if (pic.mimeMismatch) {
      caption.append(
        el('div', {
          className: 'metadata-warning',
          text: `Declared "${pic.declaredMimeType}" but looks like "${pic.detectedMimeType}"`,
        }),
      );
    }
    figure.append(caption);
    gallery.append(figure);
  }

  section.append(gallery);
  return section;
}

function formatTimestamp(seconds: number): string {
  if (seconds < 0) return '—';
  const mm = Math.floor(seconds / 60);
  const ss = (seconds % 60).toFixed(2).padStart(5, '0');
  return `${mm}:${ss}`;
}

function renderLyrics(lyrics: LyricsResult[]): HTMLElement | null {
  if (lyrics.length === 0) return null;
  const section = el('div', { className: 'metadata-lyrics' });
  section.append(el('h3', { text: 'Lyrics' }));

  for (const l of lyrics) {
    const block = el('div', { className: 'metadata-lyrics-block' });
    block.append(
      el('p', {
        className: 'metadata-note',
        text: `${l.synced ? 'Synced' : 'Unsynced'} · ${l.sourceFormat}${l.language ? ` · ${l.language}` : ''}`,
      }),
    );
    if (l.synced) {
      const list = el('ol', { className: 'metadata-lyrics-lines' });
      for (const line of l.lines) {
        const item = el('li');
        item.append(el('span', { className: 'metadata-lyrics-time', text: formatTimestamp(line.timeSeconds) }));
        item.append(el('span', { text: ` ${line.text}` }));
        list.append(item);
      }
      block.append(list);
    } else {
      const pre = el('pre', { className: 'metadata-lyrics-plain' });
      pre.textContent = l.lines.map((line) => line.text).join('\n');
      block.append(pre);
    }
    section.append(block);
  }
  return section;
}

function renderCuePoints(cuePoints: CuePointResult[]): HTMLElement | null {
  if (cuePoints.length === 0) return null;
  const section = el('div', { className: 'metadata-cuepoints' });
  section.append(el('h3', { text: `Chapters / cue points (${cuePoints.length})` }));
  const table = el('table');
  const head = el('tr');
  head.append(el('th', { text: 'Time' }), el('th', { text: 'Label' }), el('th', { text: 'Source' }));
  table.append(head);
  for (const cue of [...cuePoints].sort((a, b) => a.timeSeconds - b.timeSeconds)) {
    const row = el('tr');
    row.append(
      el('td', { text: formatTimestamp(cue.timeSeconds) }),
      el('td', { text: cue.label }),
      el('td', { text: cue.sourceFormat }),
    );
    table.append(row);
  }
  section.append(table);
  return section;
}

function renderBroadcast(result: MetadataResult): HTMLElement | null {
  if (!result.broadcast.present) return null;
  const b = result.broadcast;
  const section = el('div', { className: 'metadata-broadcast' });
  section.append(el('h3', { text: 'Broadcast (BWF)' }));
  const dl = el('dl', { className: 'metadata-fields' });
  const rows: Array<[string, string]> = [
    ['Description', b.description],
    ['Originator', b.originator],
    ['Originator reference', b.originatorReference],
    ['Origination date', b.originationDate],
    ['Origination time', b.originationTime],
    ['Time reference (samples)', String(b.timeReference)],
    ['Version', String(b.version)],
    ['UMID', b.umid],
  ];
  if (b.loudnessValueLufs !== null) rows.push(['Loudness (LUFS)', b.loudnessValueLufs.toFixed(2)]);
  if (b.loudnessRangeLu !== null) rows.push(['Loudness range (LU)', b.loudnessRangeLu.toFixed(2)]);
  if (b.maxTruePeakDbtp !== null) rows.push(['Max true peak (dBTP)', b.maxTruePeakDbtp.toFixed(2)]);
  for (const [label, value] of rows) {
    if (!value) continue;
    dl.append(fieldRow(label, value));
  }
  section.append(dl);
  return section;
}

function renderConflicts(result: MetadataResult): HTMLElement | null {
  if (result.fieldConflicts.length === 0) return null;
  const section = el('div', { className: 'metadata-conflicts metadata-warning' });
  section.append(
    el('h3', { text: 'Conflicting tags' }),
    el('p', {
      text: 'Different tagging systems in this file disagree on these fields — the value shown above is the higher-priority source; every source is listed here.',
    }),
  );
  const table = el('table');
  const head = el('tr');
  head.append(el('th', { text: 'Field' }), el('th', { text: 'Value' }), el('th', { text: 'Source' }));
  table.append(head);
  for (const { key, text, sourceFormat } of result.fieldConflicts) {
    const row = el('tr');
    row.append(el('td', { text: key }), el('td', { text }), el('td', { text: sourceFormat }));
    table.append(row);
  }
  section.append(table);
  return section;
}

function renderUnmapped(result: MetadataResult): HTMLElement | null {
  if (result.unmapped.length === 0) return null;
  const section = el('div', { className: 'metadata-unmapped' });
  section.append(
    el('h3', { text: `Raw / unmapped tags (${result.unmapped.length})` }),
    el('p', {
      className: 'metadata-note',
      text: 'Everything this parser recognised but doesn\'t map onto a named field — nothing is silently dropped.',
    }),
  );
  const table = el('table');
  const head = el('tr');
  head.append(
    el('th', { text: 'Key' }),
    el('th', { text: 'Value' }),
    el('th', { text: 'Source' }),
    el('th', { text: '' }),
  );
  table.append(head);
  for (const entry of result.unmapped) {
    const row = el('tr');
    row.append(
      el('td', { text: entry.rawKey ?? entry.key }),
      el('td', { text: entry.text }),
      el('td', { text: entry.sourceFormat }),
    );
    const copyCell = el('td');
    copyCell.append(copyButton(entry.text));
    row.append(copyCell);
    table.append(row);
  }
  section.append(table);
  return section;
}

function renderDiagnostics(result: MetadataResult): HTMLElement | null {
  if (result.diagnostics.length === 0) return null;
  const section = el('div', { className: 'metadata-diagnostics' });
  section.append(el('h3', { text: 'Parser diagnostics' }));
  const list = el('ul');
  const severityName = ['info', 'warning', 'error'] as const;
  for (const d of result.diagnostics) {
    const item = el('li', { className: `metadata-diagnostic-${severityName[d.severity] ?? 'info'}` });
    item.textContent = `[${severityName[d.severity] ?? d.severity}] ${d.message}`;
    list.append(item);
  }
  section.append(list);
  return section;
}

/** Rebuilds `container`'s contents from a fresh MetadataResult. Safe to call repeatedly (e.g. once
 *  per file load) — clears whatever was there before. */
export function renderMetadataPanel(container: HTMLElement, host: MetadataPanelHost, result: MetadataResult): void {
  const sections = [
    renderMappedFields(result),
    renderReplayGain(result),
    renderPictures(host, result.pictures),
    renderLyrics(result.lyrics),
    renderCuePoints(result.cuePoints),
    renderBroadcast(result),
    renderConflicts(result),
    renderUnmapped(result),
    renderDiagnostics(result),
  ].filter((node): node is HTMLElement => node !== null);

  container.replaceChildren(...sections);
}

/** The "no tags at all" state (M15 acceptance criterion: "produces an empty-but-valid result and a
 *  clear UI state, not an error"). */
export function renderNoMetadata(container: HTMLElement, reason?: string): void {
  container.replaceChildren(el('p', { text: reason ?? 'No tags found in this file.' }));
}
