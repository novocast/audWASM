import { describe, expect, it } from 'vitest';
import { exportToAudacityLabels, exportToCsv, exportToJson } from './exportMarkers.ts';
import type { Marker } from './model.ts';

const markers: Marker[] = [
  { id: 'b1', kind: 'bookmark', startFrame: 44100, label: 'Verse', userCreated: true },
  { id: 'c1', kind: 'clipping', startFrame: 22050, endFrame: 26460, severity: 'warning' },
];

describe('exportToJson', () => {
  it('round-trips every field', () => {
    const parsed = JSON.parse(exportToJson(markers)) as Marker[];
    expect(parsed).toEqual(markers);
  });
});

describe('exportToCsv', () => {
  it('emits a header row and one data row per marker', () => {
    const csv = exportToCsv(markers);
    const lines = csv.trim().split('\r\n');
    expect(lines).toHaveLength(3);
    expect(lines[0]).toBe('id,kind,startFrame,endFrame,channel,severity,label,userCreated');
  });

  it('quotes values containing commas', () => {
    const csv = exportToCsv([{ id: 'x', kind: 'bookmark', startFrame: 0, label: 'a, b' }]);
    expect(csv).toContain('"a, b"');
  });
});

describe('exportToAudacityLabels', () => {
  it('produces tab-separated start\\tend\\tlabel lines, in seconds, sorted by time', () => {
    const out = exportToAudacityLabels(markers, 44100);
    const lines = out.trim().split('\n');
    expect(lines).toHaveLength(2);
    // c1 (0.5s) sorts before b1 (1.0s) despite being listed second above.
    expect(lines[0]!.split('\t')).toEqual(['0.500000', '0.600000', 'clipping']);
    expect(lines[1]!.split('\t')).toEqual(['1.000000', '1.000000', 'Verse']);
  });

  it('repeats the start as the end for a point marker (Audacity point-label convention)', () => {
    const out = exportToAudacityLabels([{ id: 'p', kind: 'beat', startFrame: 44100 }], 44100);
    const [start, end] = out.trim().split('\t');
    expect(start).toBe(end);
  });
});
