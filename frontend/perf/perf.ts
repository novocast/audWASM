// M17 follow-up: automated performance test harness. See perf/index.html's comment for why this
// drives the real RenderLoop/Renderer against synthetic data rather than a decoded file, and
// tests/e2e/perf.spec.ts for the Playwright assertion that reads `#perf-result`.

import { defaultViewState, ViewStateStore } from '../src/renderer/viewState.ts';
import { createRenderer, detectBackendCapabilities, selectBackendName } from '../src/renderer/backendSelection.ts';
import { RenderLoop } from '../src/renderer/loop.ts';
import type { WaveformBinsLike, WaveformQueryResult, MarkerLike, SelectionRange } from '../src/renderer/renderer.ts';

const kSampleRate = 44100;
const kDurationSeconds = 3600; // "a 1-hour file" (M17 acceptance criteria)
const kTotalFrames = kSampleRate * kDurationSeconds;
const kRunMs = 3000; // measurement window — long enough for FrameTimeStats's 300-frame ring buffer to fill twice over at 60fps
const kPlaybackSpeedup = 120; // simulated playhead races through the hour in ~30s of wall-clock so follow-mode tracking stays exercised

// Deterministic procedural "waveform" — two overlaid sinusoids, no engine/decode dependency.
// Recomputed per query (matching the real query-on-demand path) rather than precomputed once, so
// the per-frame cost this measures includes whatever a real bins buffer of this size would cost
// to read.
function makeSyntheticBins(perChannelBinCount: number, channels: number): WaveformBinsLike {
  const total = perChannelBinCount * channels;
  return {
    binCount: total,
    min: (i) => Math.sin(i * 0.017) * 0.6 + Math.sin(i * 0.31) * 0.3 - 0.05,
    max: (i) => Math.sin(i * 0.017) * 0.6 + Math.sin(i * 0.31) * 0.3 + 0.05,
    rms: (i) => Math.abs(Math.sin(i * 0.017) * 0.6) * 0.7,
    absPeak: (i) => Math.abs(Math.sin(i * 0.017) * 0.6) + 0.05,
  };
}

function queryWaveform(binCountDevicePx: number): WaveformQueryResult {
  const channels = 2;
  return {
    bins: makeSyntheticBins(binCountDevicePx, channels),
    channels,
    framesPerBin: kTotalFrames / binCountDevicePx,
    isComplete: true,
    isRawPcm: false,
  };
}

const kMarkers: MarkerLike[] = Array.from({ length: 20 }, (_, i) => ({
  id: `marker-${i}`,
  frame: (kTotalFrames * i) / 20,
  label: `Marker ${i}`,
}));

const kSelection: SelectionRange = { startFrame: kTotalFrames * 0.1, endFrame: kTotalFrames * 0.35 };

async function main(): Promise<void> {
  const container = document.querySelector<HTMLElement>('#waveform-container')!;
  const resultEl = document.querySelector<HTMLElement>('#perf-result')!;

  const backendCapabilities = detectBackendCapabilities();
  const backendName = selectBackendName('auto', backendCapabilities);
  const renderer = createRenderer(backendName);
  renderer.attach(container);

  const viewStore = new ViewStateStore(defaultViewState({ channelLayout: 'split', amplitudeScale: { type: 'db', param: 1 } }), {
    totalFrames: kTotalFrames,
    minFramesPerPixel: 1 / 64,
  });
  viewStore.dispatch({ type: 'zoomToRange', startFrame: 0, endFrame: kTotalFrames });

  const startMs = performance.now();

  const loop = new RenderLoop({
    hostElement: container,
    renderer,
    viewStore,
    getPlayheadFrame: () => ((performance.now() - startMs) / 1000) * kSampleRate * kPlaybackSpeedup,
    isPlaying: () => true,
    sampleRate: () => kSampleRate,
    trackDurationFrames: () => kTotalFrames,
    queryWaveform: (view, binCountDevicePx) => queryWaveform(binCountDevicePx),
    getSelection: () => kSelection,
    getMarkers: () => kMarkers,
    onBeforeFrame: () => {
      // "all overlays on" (M17 acceptance criteria) — hover feedback changing every frame is the
      // cheapest possible way to keep the interaction-feedback layer genuinely live rather than
      // static, matching a real user hovering while a track plays.
      loop.setHover({ xCss: (performance.now() % 800), label: 'hover' });
    },
    onFrameStats: (stats) => {
      resultEl.textContent = JSON.stringify(stats);
    },
  });
  loop.start();

  setTimeout(() => {
    loop.stop();
    resultEl.dataset.done = 'true';
  }, kRunMs);
}

void main();
