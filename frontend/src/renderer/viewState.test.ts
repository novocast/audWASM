import { describe, expect, it } from 'vitest';
import { defaultViewState, followTargetStartFrame, viewReducer, type ViewLimits, type ViewState } from './viewState.ts';

const kSampleRate = 44100;

function limitsFor(totalFrames: number, overrides: Partial<ViewLimits> = {}): ViewLimits {
  return { totalFrames, minFramesPerPixel: 1 / 64, ...overrides };
}

describe('viewReducer resize/clamping', () => {
  it('centres the view when the track is narrower than the viewport at max zoom-out', () => {
    const totalFrames = 1000;
    const limits = limitsFor(totalFrames);
    let state = defaultViewState({ framesPerPixel: totalFrames / 100 });
    state = viewReducer(state, { type: 'resize', widthCss: 2000, heightCss: 100 }, limits);
    // framesPerPixel clamps to totalFrames/width when the whole track already fits.
    expect(state.framesPerPixel).toBeCloseTo(totalFrames / 2000, 9);
  });

  it('never lets framesPerPixel go below minFramesPerPixel', () => {
    const limits = limitsFor(10_000_000);
    let state = defaultViewState();
    state = viewReducer(state, { type: 'resize', widthCss: 800, heightCss: 100 }, limits);
    state = viewReducer(state, { type: 'setFramesPerPixel', framesPerPixel: 1e-9 }, limits);
    expect(state.framesPerPixel).toBeGreaterThanOrEqual(limits.minFramesPerPixel);
  });

  it('never lets startFrame go negative or past the end for a huge (3-hour) file at 800px', () => {
    const totalFrames = 3 * 3600 * kSampleRate;
    const limits = limitsFor(totalFrames);
    let state = defaultViewState({ framesPerPixel: totalFrames / 800 });
    state = viewReducer(state, { type: 'resize', widthCss: 800, heightCss: 100 }, limits);
    state = viewReducer(state, { type: 'pan', deltaFrames: -1e12 }, limits);
    expect(state.startFrame).toBeGreaterThanOrEqual(0);
    state = viewReducer(state, { type: 'pan', deltaFrames: 1e12 }, limits);
    const visibleFrames = state.framesPerPixel * state.widthCss;
    expect(state.startFrame).toBeLessThanOrEqual(totalFrames - visibleFrames + 1e-6);
  });
});

describe('viewReducer zoomAt anchoring', () => {
  it('keeps the anchor pixel mapped to the same frame after a zoomAt action', () => {
    const limits = limitsFor(1_000_000);
    let state = defaultViewState({ startFrame: 5000, framesPerPixel: 4 });
    state = viewReducer(state, { type: 'resize', widthCss: 1000, heightCss: 100 }, limits);
    const anchorPixel = 300;
    const before = state.startFrame + anchorPixel * state.framesPerPixel;
    state = viewReducer(state, { type: 'zoomAt', anchorPixel, factor: 0.5 }, limits);
    const after = state.startFrame + anchorPixel * state.framesPerPixel;
    expect(after).toBeCloseTo(before, 3);
  });
});

describe('viewReducer channel layout / amplitude scale / vertical zoom', () => {
  it('sets channel layout and amplitude scale directly', () => {
    const limits = limitsFor(1000);
    let state = defaultViewState();
    state = viewReducer(state, { type: 'setChannelLayout', channelLayout: 'midSide' }, limits);
    expect(state.channelLayout).toBe('midSide');
    state = viewReducer(state, { type: 'setAmplitudeScale', amplitudeScale: { type: 'db', param: 1 } }, limits);
    expect(state.amplitudeScale.type).toBe('db');
  });

  it('clamps vertical zoom to the configured limits', () => {
    const limits = limitsFor(1000, { minVerticalZoom: 0.5, maxVerticalZoom: 4 });
    let state = defaultViewState();
    state = viewReducer(state, { type: 'setVerticalZoom', verticalZoom: 100 }, limits);
    expect(state.verticalZoom).toBe(4);
    state = viewReducer(state, { type: 'setVerticalZoom', verticalZoom: -5 }, limits);
    expect(state.verticalZoom).toBe(0.5);
  });
});

describe('follow suspension', () => {
  it('a manual pan suspends an active follow mode', () => {
    const limits = limitsFor(1_000_000);
    let state = defaultViewState({ followPlayhead: 'centre' });
    state = viewReducer(state, { type: 'resize', widthCss: 800, heightCss: 100 }, limits);
    state = viewReducer(state, { type: 'pan', deltaFrames: 500 }, limits);
    expect(state.followSuspended).toBe(true);
  });

  it('a follow-driven move does not suspend follow', () => {
    const limits = limitsFor(1_000_000);
    let state = defaultViewState({ followPlayhead: 'centre' });
    state = viewReducer(state, { type: 'resize', widthCss: 800, heightCss: 100 }, limits);
    state = viewReducer(state, { type: 'followMove', startFrame: 900 }, limits);
    expect(state.followSuspended).toBe(false);
  });

  it('resumeFollow clears suspension', () => {
    const limits = limitsFor(1_000_000);
    let state: ViewState = defaultViewState({ followPlayhead: 'page', followSuspended: true });
    state = viewReducer(state, { type: 'resumeFollow' }, limits);
    expect(state.followSuspended).toBe(false);
  });
});

describe('followTargetStartFrame', () => {
  it('returns null when follow is off', () => {
    const limits = limitsFor(100_000);
    const state = defaultViewState({ widthCss: 800, framesPerPixel: 10, followPlayhead: 'off' });
    expect(followTargetStartFrame(state, 50_000, limits)).toBeNull();
  });

  it('returns null while follow is suspended', () => {
    const limits = limitsFor(100_000);
    const state = defaultViewState({ widthCss: 800, framesPerPixel: 10, followPlayhead: 'centre', followSuspended: true });
    expect(followTargetStartFrame(state, 50_000, limits)).toBeNull();
  });

  it('centre mode keeps the playhead in the middle of the view', () => {
    const limits = limitsFor(1_000_000);
    const state = defaultViewState({ widthCss: 800, framesPerPixel: 10, followPlayhead: 'centre' });
    const target = followTargetStartFrame(state, 50_000, limits);
    expect(target).not.toBeNull();
    const visibleFrames = state.framesPerPixel * state.widthCss;
    expect(target!).toBeCloseTo(50_000 - visibleFrames / 2, 3);
  });

  it('page mode only jumps once the playhead leaves the visible range', () => {
    const limits = limitsFor(1_000_000);
    const state = defaultViewState({ startFrame: 0, widthCss: 800, framesPerPixel: 10, followPlayhead: 'page' });
    const visibleFrames = state.framesPerPixel * state.widthCss;
    expect(followTargetStartFrame(state, visibleFrames / 2, limits)).toBeNull();
    expect(followTargetStartFrame(state, visibleFrames + 1, limits)).not.toBeNull();
  });
});
