// M09's statistics viewer — surfaces the per-channel/stereo numbers that have no time axis (peak,
// RMS, crest factor, dynamic range, correlation, bit depth/dither) and so don't belong on the
// timeline as markers. Plain DOM construction, mirroring metadataPanel.ts's established convention
// (no framework in this repo).

import type { StatisticsResult } from '../../bindings/wasm/engine.ts';

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  opts?: { text?: string; className?: string },
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (opts?.text !== undefined) node.textContent = opts.text;
  if (opts?.className) node.className = opts.className;
  return node;
}

function fieldRow(label: string, value: string): HTMLElement {
  const row = el('div', { className: 'statistics-field' });
  row.append(el('dt', { text: label }), el('dd', { text: value }));
  return row;
}

function renderOverall(result: StatisticsResult): HTMLElement {
  const dl = el('dl', { className: 'statistics-fields' });
  dl.append(
    fieldRow('Sample rate', `${result.sampleRate} Hz`),
    fieldRow('Channels', String(result.channelCount)),
    fieldRow('Crest factor', `${result.crestFactorDb.toFixed(1)} dB`),
    fieldRow('Dynamic range (DR)', result.dynamicRangeDr.toFixed(1)),
  );
  return dl;
}

function renderChannels(result: StatisticsResult): HTMLElement {
  const table = el('table', { className: 'statistics-channels' });
  const head = el('tr');
  for (const label of ['Ch', 'Peak', 'RMS', 'DC offset', 'Crest', 'Bit depth', 'Dither']) {
    head.append(el('th', { text: label }));
  }
  table.append(head);
  result.channels.forEach((c, i) => {
    const row = el('tr');
    row.append(
      el('td', { text: String(i) }),
      el('td', { text: `${c.peakDbfs.toFixed(1)} dBFS` }),
      el('td', { text: `${c.rmsDbfs.toFixed(1)} dBFS` }),
      el('td', { text: `${c.dcOffset.toFixed(4)}` }),
      el('td', { text: `${c.crestFactorDb.toFixed(1)} dB` }),
      el('td', { text: c.bitDepthDescription }),
      el('td', { text: c.ditherLikely ? `likely (${(c.ditherConfidence * 100).toFixed(0)}%)` : 'unlikely' }),
    );
    table.append(row);
  });
  return table;
}

function renderStereo(result: StatisticsResult): HTMLElement | null {
  if (!result.stereo) return null;
  const dl = el('dl', { className: 'statistics-fields' });
  dl.append(
    fieldRow('Correlation', result.stereo.correlation.toFixed(2)),
    fieldRow('Balance', `${result.stereo.balanceDb.toFixed(1)} dB`),
    fieldRow('Mono compatibility', `${result.stereo.monoCompatibilityDb.toFixed(1)} dB`),
  );
  return dl;
}

/** Rebuilds `container`'s contents from a fresh StatisticsResult. Safe to call repeatedly (e.g.
 *  once per file load). */
export function renderStatisticsPanel(container: HTMLElement, result: StatisticsResult): void {
  const sections = [renderOverall(result), renderChannels(result), renderStereo(result)].filter(
    (node): node is HTMLElement => node !== null,
  );
  container.replaceChildren(...sections);
}

export function renderNoStatistics(container: HTMLElement, reason?: string): void {
  container.replaceChildren(el('p', { text: reason ?? 'No statistics available for this file.' }));
}
