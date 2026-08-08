// M18 export: CSV, JSON, and Audacity label-track format ("Decision — markers export to CSV,
// JSON, and Audacity label track format. The last one is nearly free (tab-separated
// `start\tend\tlabel`) and immediately makes the tool interoperable with a workflow people already
// have"). Reaper region CSV / `.cue` sheets are noted in the doc as "worth considering" but not
// committed to — deferred, same as the doc leaves them.

import type { Marker } from './model.ts';

function csvEscape(value: string): string {
  if (/[",\n]/.test(value)) return `"${value.replace(/"/g, '""')}"`;
  return value;
}

export function exportToJson(markers: readonly Marker[]): string {
  return JSON.stringify(markers, null, 2);
}

const kCsvColumns = ['id', 'kind', 'startFrame', 'endFrame', 'channel', 'severity', 'label', 'userCreated'] as const;

export function exportToCsv(markers: readonly Marker[]): string {
  const rows = [kCsvColumns.join(',')];
  for (const m of markers) {
    rows.push(
      kCsvColumns
        .map((col) => {
          const v = m[col];
          return v === undefined ? '' : csvEscape(String(v));
        })
        .join(','),
    );
  }
  return rows.join('\r\n') + '\r\n';
}

/**
 * Audacity label track format: one label per line, tab-separated `start\tend\tlabel`, times in
 * seconds. A point marker repeats its start as its end (Audacity's own convention for point
 * labels). Markers are emitted in `startFrame` order, which is what Audacity's importer expects.
 */
export function exportToAudacityLabels(markers: readonly Marker[], sampleRate: number): string {
  const sorted = [...markers].sort((a, b) => a.startFrame - b.startFrame);
  const lines = sorted.map((m) => {
    const start = m.startFrame / sampleRate;
    const end = (m.endFrame ?? m.startFrame) / sampleRate;
    const label = (m.label ?? m.kind).replace(/[\t\n]/g, ' ');
    return `${formatAudacityTime(start)}\t${formatAudacityTime(end)}\t${label}`;
  });
  return lines.join('\n') + (lines.length > 0 ? '\n' : '');
}

/** Audacity writes plain decimal seconds (not scientific notation, which its importer rejects for
 *  very small/large values) with enough precision to stay sample-accurate at typical sample rates. */
function formatAudacityTime(seconds: number): string {
  return seconds.toFixed(6);
}
