import { describe, expect, it } from 'vitest';
import {
  beatsToMarkers,
  clippingToMarkers,
  dcToMarkers,
  loudnessToCurve,
  silenceToMarkers,
  transientsToMarkers,
} from './analysisMarkers.ts';
import type { BeatsResult, ClippingResult, DcResult, SilenceModeResult, TransientsResult } from '../../bindings/wasm/engine.ts';

function beatsResult(overrides: Partial<BeatsResult> = {}): BeatsResult {
  return {
    primaryBpm: 128,
    tempoConfidence: 0.9,
    phaseConfidence: 0.8,
    tempoIsStable: true,
    odfHopSeconds: 0.01,
    reportJson: '{}',
    onsets: [],
    beats: [],
    alternatives: [],
    odf: new Float32Array(0),
    tempoSeries: new Float32Array(0),
    ...overrides,
  };
}

describe('beatsToMarkers', () => {
  it('classifies beatIndexInBar === 0 as a downbeat, everything else as a beat', () => {
    const result = beatsResult({
      beats: [
        { timeSeconds: 0, frame: 0, confidence: 0.9, beatIndexInBar: 0 },
        { timeSeconds: 0.5, frame: 24000, confidence: 0.8, beatIndexInBar: 1 },
      ],
    });
    const markers = beatsToMarkers(result);
    expect(markers).toHaveLength(2);
    expect(markers[0]!.kind).toBe('downbeat');
    expect(markers[0]!.startFrame).toBe(0);
    expect(markers[1]!.kind).toBe('beat');
    expect(markers[1]!.startFrame).toBe(24000);
  });

  it('produces unique ids even for beats at the same frame', () => {
    const result = beatsResult({
      beats: [
        { timeSeconds: 0, frame: 100, confidence: 0.5, beatIndexInBar: -1 },
        { timeSeconds: 0, frame: 100, confidence: 0.5, beatIndexInBar: -1 },
      ],
    });
    const markers = beatsToMarkers(result);
    expect(new Set(markers.map((m) => m.id)).size).toBe(2);
  });
});

function clippingResult(overrides: Partial<ClippingResult> = {}): ClippingResult {
  return {
    totalClippedSamples: 0,
    clippedFraction: 0,
    maxOvershootDb: 0,
    flatTopRatio: 0,
    meanPlateauLength: 0,
    heavyLimitingLikely: false,
    containerBitDepth: 16,
    reportJson: '{}',
    eventCount: [0, 0, 0, 0],
    events: [],
    eventsCapped: false,
    densitySeries: new Float32Array(0),
    densityBinFrames: 1024,
    ...overrides,
  };
}

describe('clippingToMarkers', () => {
  it('marks OverFullScale/InterSamplePeak as error, Digital/NearClip as warning', () => {
    const result = clippingResult({
      events: [
        { beginFrame: 0, endFrame: 10, startSeconds: 0, endSeconds: 0.001, channel: 0, kind: 0, peakValue: 1, peakDbfs: 0, sampleCount: 10 }, // Digital
        { beginFrame: 20, endFrame: 21, startSeconds: 0, endSeconds: 0, channel: 0, kind: 1, peakValue: 1.2, peakDbfs: 1.5, sampleCount: 1 }, // OverFullScale
        { beginFrame: 30, endFrame: 31, startSeconds: 0, endSeconds: 0, channel: 0, kind: 2, peakValue: 0.95, peakDbfs: -0.4, sampleCount: 1 }, // NearClip
        { beginFrame: 40, endFrame: 41, startSeconds: 0, endSeconds: 0, channel: 0, kind: 3, peakValue: 1.1, peakDbfs: 0.8, sampleCount: 1 }, // InterSamplePeak
      ],
    });
    const markers = clippingToMarkers(result);
    expect(markers.map((m) => m.severity)).toEqual(['warning', 'error', 'warning', 'error']);
  });

  it('omits endFrame for point events (endFrame === beginFrame)', () => {
    const result = clippingResult({
      events: [{ beginFrame: 30, endFrame: 30, startSeconds: 0, endSeconds: 0, channel: 0, kind: 3, peakValue: 1.1, peakDbfs: 0.8, sampleCount: 1 }],
    });
    expect(clippingToMarkers(result)[0]!.endFrame).toBeUndefined();
  });
});

function dcResult(overrides: Partial<DcResult> = {}): DcResult {
  return {
    significanceThresholdDbfs: -60,
    anySignificant: false,
    windowSeconds: 1,
    reportJson: '{}',
    channels: [],
    windowSeries: new Float32Array(0),
    windowSeriesChannelCount: 1,
    ...overrides,
  };
}

describe('dcToMarkers', () => {
  it('skips channels with pattern None and spans the full track for significant ones', () => {
    const result = dcResult({
      channels: [
        { offsetLinear: 0, offsetDbfs: -90, offsetPercent: 0, pattern: 0, minWindowOffset: 0, maxWindowOffset: 0, headroomLostDb: 0, peakAfterCorrectionDbfs: 0, recommendedHighpassHz: 0, stepLocations: [] },
        { offsetLinear: 0.01, offsetDbfs: -40, offsetPercent: 1, pattern: 1, minWindowOffset: 0, maxWindowOffset: 0, headroomLostDb: 1.5, peakAfterCorrectionDbfs: 0, recommendedHighpassHz: 0, stepLocations: [] },
      ],
    });
    const markers = dcToMarkers(result, 44100);
    expect(markers).toHaveLength(1);
    expect(markers[0]!.startFrame).toBe(0);
    expect(markers[0]!.endFrame).toBe(44100);
    expect(markers[0]!.severity).toBe('warning');
  });
});

function transientsResult(overrides: Partial<TransientsResult> = {}): TransientsResult {
  return { reportJson: '{}', transients: [], defects: [], countByClass: [], ...overrides };
}

function transient(over: Partial<TransientsResult['transients'][number]>) {
  return {
    startFrame: 0,
    attackFrame: 0,
    startSeconds: 0,
    attackSeconds: 0,
    classification: 'unclassified' as const,
    classConfidence: 0.5,
    strength: 1,
    peakDbfs: 0,
    attackTimeMs: 0,
    decayTimeMs: 0,
    spectralCentroidHz: 0,
    spectralFlatness: 0,
    bandEnergyRatio: [0, 0, 0, 0] as [number, number, number, number],
    ...over,
  };
}

describe('transientsToMarkers', () => {
  it('splits transients and defects into separate lists, defects severity by classification', () => {
    const result = transientsResult({
      transients: [transient({ classification: 'kick', attackFrame: 10 })],
      defects: [transient({ classification: 'click', attackFrame: 20 }), transient({ classification: 'dropout', attackFrame: 30 })],
    });
    const { transients, defects } = transientsToMarkers(result);
    expect(transients).toHaveLength(1);
    expect(transients[0]!.severity).toBeUndefined();
    expect(defects.map((d) => d.severity)).toEqual(['warning', 'error']);
  });
});

describe('silenceToMarkers', () => {
  it('maps regions to silence markers with start/end frames preserved', () => {
    const mode: SilenceModeResult = {
      regions: [
        { beginFrame: 100, endFrame: 200, startSeconds: 0, endSeconds: 0, kind: 0, position: 0, peakDbfsWithin: -80, rmsDbfsWithin: -90, channelMask: 1 },
      ],
      leadingSilenceSeconds: 0,
      trailingSilenceSeconds: 0,
      totalSilenceSeconds: 0,
      silenceFraction: 0,
      parametersUsed: { thresholdDb: -60, minDurationMs: 500, mergeGapMs: 100, channelModeAny: false, useHysteresis: true, hysteresisDb: 3 },
    };
    const markers = silenceToMarkers(mode);
    expect(markers).toHaveLength(1);
    expect(markers[0]!.startFrame).toBe(100);
    expect(markers[0]!.endFrame).toBe(200);
    expect(markers[0]!.kind).toBe('silence');
  });
});

describe('loudnessToCurve', () => {
  it('ignores non-finite windows when computing the display range', () => {
    const curve = loudnessToCurve(Float32Array.from([-20, -Infinity, -10]), 48000);
    expect(curve.minValue).toBe(-23);
    expect(curve.maxValue).toBe(-7);
    expect(curve.framesPerSample).toBe(4800); // 100ms at 48kHz
  });

  it('falls back to a fixed range when every window is non-finite', () => {
    const curve = loudnessToCurve(Float32Array.from([-Infinity, -Infinity]), 48000);
    expect(curve.minValue).toBe(-60);
    expect(curve.maxValue).toBe(0);
  });
});
