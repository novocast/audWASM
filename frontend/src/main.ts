import { AudioEngine, type DecodeSession, type Waveform } from '../../bindings/wasm/engine.ts';
import type { WaveformChannelSelector } from '../../bindings/wasm/aud_wasm.d.ts';
import { renderMetadataPanel, renderNoMetadata } from './metadataPanel.ts';
import { TransportClient, type TransportObservableState } from './playback/transportClient.ts';
import { defaultViewState, ViewStateStore, type ChannelLayout, type FollowMode, type AmplitudeScaleType, type ViewState, type SpectrogramColorMap } from './renderer/viewState.ts';
import { createRenderer, detectBackendCapabilities, readBackendOverride, selectBackendName, setBackendOverride, type BackendOverride } from './renderer/backendSelection.ts';
import { RenderLoop } from './renderer/loop.ts';
import type { WaveformQueryResult } from './renderer/renderer.ts';
import { attachInteraction, type SelectionRange } from './renderer/interaction.ts';
import type { HoverFeedback } from './renderer/playheadOverlay.ts';
import { attachAccessibleSummary, buildAccessibleSummary } from './renderer/accessibleSummary.ts';
import { frameToSeconds, pixelToHz, pixelToSeconds } from './renderer/coords.ts';
import { applyThemeOverride, readThemeOverride, setThemeOverride, type ThemeOverride } from './renderer/themeOverride.ts';
import { SpectrogramTileManager, chooseFftSize } from './renderer/spectrogram/tileManager.ts';
import type { FreqAxis } from './renderer/coords.ts';
import type { SpectrogramDecimationMode } from './renderer/viewState.ts';

const kMinFramesPerPixel = 1 / 64;

function channelsModeFor(layout: ChannelLayout): WaveformChannelSelector {
  switch (layout) {
    case 'monoSum':
      return 1;
    case 'midSide':
      return 2;
    default:
      return 0; // 'split' and 'overlaid' both want one band per source channel.
  }
}

async function main(): Promise<void> {
  const statusEl = document.querySelector<HTMLParagraphElement>('#status')!;
  const infoEl = document.querySelector<HTMLDListElement>('#build-info')!;

  let engine: AudioEngine;
  try {
    engine = await AudioEngine.create();
    const pass = engine.runSelfTest();

    statusEl.textContent = pass ? 'Engine self-test: PASS' : 'Engine self-test: FAIL';
    statusEl.style.color = pass ? 'seagreen' : 'crimson';

    const info = engine.buildInfo;
    infoEl.innerHTML = '';
    for (const [key, value] of Object.entries(info)) {
      const dt = document.createElement('dt');
      dt.textContent = key;
      const dd = document.createElement('dd');
      dd.textContent = String(value);
      infoEl.append(dt, dd);
    }
  } catch (err) {
    statusEl.textContent = `Engine failed to load: ${(err as Error).message}`;
    statusEl.style.color = 'crimson';
    return;
  }

  setupPlayback(engine);
}

// Demo-scope simplification: decode runs on the main thread here rather than in
// frontend/workers/decodeWorker.ts. M03's Transport (and the AudioBuffer it reads from) must live
// in the same WASM module instance/realm as the DecodeSession that owns the decoded PCM — wiring
// that cleanly across the worker/main-thread boundary for a *playback* session is a scheduling
// concern the M02 doc explicitly defers to M20 ("M20 defines the scheduler"). The engine/binding/TS
// sink code in playback/ is written to work either way; only this demo wiring takes the simpler
// same-thread path.
function setupPlayback(engine: AudioEngine): void {
  const section = document.querySelector<HTMLElement>('#playback')!;
  const fileInput = document.querySelector<HTMLInputElement>('#file-input')!;
  const playbackStatus = document.querySelector<HTMLParagraphElement>('#playback-status')!;
  const playBtn = document.querySelector<HTMLButtonElement>('#btn-play')!;
  const pauseBtn = document.querySelector<HTMLButtonElement>('#btn-pause')!;
  const seekSlider = document.querySelector<HTMLInputElement>('#seek-slider')!;
  const gainSlider = document.querySelector<HTMLInputElement>('#gain-slider')!;
  const loopToggle = document.querySelector<HTMLInputElement>('#loop-toggle')!;
  const positionReadout = document.querySelector<HTMLParagraphElement>('#position-readout')!;
  const waveformContainer = document.querySelector<HTMLElement>('#waveform-container')!;
  const backendSelect = document.querySelector<HTMLSelectElement>('#renderer-backend')!;
  const channelLayoutSelect = document.querySelector<HTMLSelectElement>('#renderer-channel-layout')!;
  const amplitudeScaleSelect = document.querySelector<HTMLSelectElement>('#renderer-amplitude-scale')!;
  const followModeSelect = document.querySelector<HTMLSelectElement>('#renderer-follow-mode')!;
  const themeSelect = document.querySelector<HTMLSelectElement>('#renderer-theme')!;
  const backendActiveLabel = document.querySelector<HTMLElement>('#renderer-backend-active')!;
  const frameStatsLabel = document.querySelector<HTMLElement>('#renderer-frame-stats')!;
  const colorMapSelect = document.querySelector<HTMLSelectElement>('#spectrogram-colormap')!;
  const freqAxisSelect = document.querySelector<HTMLSelectElement>('#spectrogram-freq-axis')!;
  const floorDbInput = document.querySelector<HTMLInputElement>('#spectrogram-floor-db')!;
  const gainDbInput = document.querySelector<HTMLInputElement>('#spectrogram-gain-db')!;
  const gammaInput = document.querySelector<HTMLInputElement>('#spectrogram-gamma')!;
  const decimationSelect = document.querySelector<HTMLSelectElement>('#spectrogram-decimation')!;
  const cacheStatsLabel = document.querySelector<HTMLElement>('#spectrogram-cache-stats')!;
  const spectrogramReadout = document.querySelector<HTMLParagraphElement>('#spectrogram-readout')!;
  const metadataSection = document.querySelector<HTMLElement>('#metadata')!;
  const metadataContent = document.querySelector<HTMLElement>('#metadata-content')!;

  section.hidden = false;

  // Applied before mountRenderer() below so the RenderLoop's first readThemeTokens() (loop.ts's
  // constructor) already sees the override rather than briefly flashing the OS-default theme.
  const themeOverride = readThemeOverride();
  applyThemeOverride(themeOverride);
  themeSelect.value = themeOverride;

  let client: TransportClient | null = null;
  let waveform: Waveform | null = null;
  let session: DecodeSession | null = null;
  let sourceSampleRate = 1;
  let seeking = false;
  let latestTransportState: TransportObservableState | null = null;
  let selection: SelectionRange | null = null;

  const backendCapabilities = detectBackendCapabilities();
  const viewStore = new ViewStateStore(defaultViewState(), { totalFrames: 0, minFramesPerPixel: kMinFramesPerPixel });

  // M07: the spectrogram's tile-generation worker — the first genuinely-wired worker in the app.
  // See frontend/workers/spectrogramWorker.ts for why it gets its own PCM copy/WASM module
  // instance rather than sharing this thread's engine.
  const spectrogramWorker = new Worker(new URL('../workers/spectrogramWorker.ts', import.meta.url), { type: 'module' });
  const tileManager = new SpectrogramTileManager(spectrogramWorker, () => loop?.invalidateSpectrogram());
  let lastAppliedFftSize: number | null = null;
  let lastAppliedConfigKey = '';

  function freqAxisEnum(axis: FreqAxis): number {
    return { linear: 0, log: 1, mel: 2, bark: 3 }[axis];
  }
  function decimationEnum(mode: SpectrogramDecimationMode): number {
    return mode === 'max' ? 0 : 1;
  }

  /** Pushes the current UI/adaptive config to the worker whenever the *effective* fftSize changes
   *  (adaptive or overridden) or the user changes freq axis / decimation — window/scaling/minHz/
   *  the quantisation floor-ceil are fixed for this demo (M07 doesn't require exposing every knob
   *  to be a working spectrogram). Colour map / display floor / gain / gamma are deliberately NOT
   *  sent here — those are shader-only (viewState.spectrogram), applied instantly with no worker
   *  round-trip at all (M07 acceptance criteria). */
  function syncSpectrogramConfig(): void {
    if (!tileManager.isReady) return;
    const view = viewStore.state;
    const fftSize = view.spectrogram.fftSizeOverride ?? chooseFftSize(view.framesPerPixel, lastAppliedFftSize ?? 8192);
    const key = `${fftSize}:${view.spectrogram.freqAxis}:${view.spectrogram.decimation}`;
    if (key === lastAppliedConfigKey) return;
    lastAppliedFftSize = fftSize;
    lastAppliedConfigKey = key;
    tileManager.setConfig(
      fftSize,
      1 /* Hann */,
      1 /* Amplitude */,
      freqAxisEnum(view.spectrogram.freqAxis),
      decimationEnum(view.spectrogram.decimation),
      20 /* minHz */,
      -96 /* quantisation floorDb */,
      0 /* quantisation ceilDb */,
    );
  }

  const setSummaryText = attachAccessibleSummary(waveformContainer, waveformContainer);
  function updateAccessibleSummary(): void {
    if (!session) return;
    const view = viewStore.state;
    setSummaryText(
      buildAccessibleSummary({
        durationSeconds: frameToSeconds(Math.max(session.decodedFrameCount, 0), sourceSampleRate),
        sampleRate: sourceSampleRate,
        channelCount: waveform?.channelCount ?? session.streamInfo.channels,
        visibleStartSeconds: frameToSeconds(view.startFrame, sourceSampleRate),
        visibleEndSeconds: frameToSeconds(view.startFrame + view.framesPerPixel * view.widthCss, sourceSampleRate),
      }),
    );
  }

  function queryWaveform(view: ViewState, binCountDevicePx: number): WaveformQueryResult | null {
    if (!waveform || binCountDevicePx < 1) return null;
    const beginFrame = Math.max(0, Math.round(view.startFrame));
    const endFrame = Math.max(beginFrame + 1, Math.round(view.startFrame + view.framesPerPixel * view.widthCss));
    try {
      return waveform.query(channelsModeFor(view.channelLayout), beginFrame, endFrame, binCountDevicePx);
    } catch {
      return null; // e.g. queried before the first chunk has landed.
    }
  }

  let loop: RenderLoop | null = null;
  function mountRenderer(override: BackendOverride): void {
    loop?.dispose();
    const backendName = selectBackendName(override, backendCapabilities);
    const renderer = createRenderer(backendName);
    renderer.attach(waveformContainer);
    backendActiveLabel.textContent = `active: ${backendName}`;

    loop = new RenderLoop({
      hostElement: waveformContainer,
      renderer,
      viewStore,
      getPlayheadFrame: () => (latestTransportState ? latestTransportState.positionSeconds * sourceSampleRate : null),
      isPlaying: () => latestTransportState?.status === 'playing',
      sampleRate: () => sourceSampleRate,
      trackDurationFrames: () => Math.max(session?.decodedFrameCount ?? 0, 0),
      queryWaveform,
      getSelection: () => selection,
      getMarkers: () => [],
      getSpectrogramSource: () => (tileManager.isReady ? tileManager : null),
      onBeforeFrame: (nowMs) => {
        if (client) {
          latestTransportState = client.tick(nowMs);
          render(latestTransportState);
        }
        syncSpectrogramConfig();
        tileManager.update(viewStore.state);
        if (tileManager.isReady) {
          cacheStatsLabel.textContent = `spectrogram: ${tileManager.residentTileCount} tiles resident`;
        }
      },
      onFrameStats: (stats) => {
        frameStatsLabel.textContent = `avg ${stats.averageMs.toFixed(2)}ms p99 ${stats.p99Ms.toFixed(2)}ms`;
      },
    });
    loop.start();
  }

  mountRenderer(readBackendOverride());
  backendSelect.value = readBackendOverride();

  // M07 "Cursor readout": debounced to actual hover-idle, not every mousemove, since queryPoint is
  // a real WASM call (a fresh centred STFT frame + peak interpolation) — cheap once, expensive at
  // 60+ calls/second while the mouse just moves across the view.
  let hoverReadoutTimer: ReturnType<typeof setTimeout> | null = null;
  function updateSpectrogramReadout(hover: HoverFeedback | null): void {
    if (hoverReadoutTimer !== null) {
      clearTimeout(hoverReadoutTimer);
      hoverReadoutTimer = null;
    }
    if (!hover || hover.yCss === undefined || !tileManager.isReady) return;

    hoverReadoutTimer = setTimeout(() => {
      const view = viewStore.state;
      const heightCss = view.heightCss;
      const axis = view.spectrogram.freqAxis;
      const nyquistHz = sourceSampleRate / 2;
      const minHz = axis === 'linear' ? 0 : 20;
      const targetHz = pixelToHz(hover.yCss!, { axis, minHz, maxHz: nyquistHz, heightCss });
      const timeSeconds = pixelToSeconds(hover.xCss, view, sourceSampleRate);

      tileManager
        .queryPoint(0, timeSeconds, targetHz)
        .then((result) => {
          spectrogramReadout.textContent =
            `t=${timeSeconds.toFixed(3)}s  f=${result.frequencyHz.toFixed(1)}Hz  ${result.magnitudeDb.toFixed(1)}dB`;
        })
        .catch(() => {
          // Point query before PCM/config is ready, or an out-of-range time — not worth surfacing.
        });
    }, 120);
  }

  attachInteraction(
    waveformContainer,
    { rulerHeightCss: 24 },
    {
      getView: () => viewStore.state,
      getLimits: () => viewStore.limits,
      getMarkers: () => [],
      dispatch: (action) => viewStore.dispatch(action),
      onSeek: (frame) => client?.seekToSeconds(frameToSeconds(Math.max(frame, 0), sourceSampleRate)),
      onSelectionChange: (range) => {
        selection = range;
        loop?.invalidateSelection();
      },
      onHover: (hover) => {
        loop?.setHover(hover);
        updateSpectrogramReadout(hover);
      },
      onDragGhost: (frame) => loop?.setDragGhost(frame),
      onTogglePlay: () => {
        if (latestTransportState?.status === 'playing') client?.pause();
        else void client?.play();
      },
      onReCentre: () => viewStore.dispatch({ type: 'resumeFollow' }),
      formatHoverLabel: (frame) => formatSeconds(frameToSeconds(frame, sourceSampleRate)),
    },
  );

  backendSelect.addEventListener('change', () => {
    const override = backendSelect.value as BackendOverride;
    setBackendOverride(override);
    mountRenderer(override);
  });
  channelLayoutSelect.addEventListener('change', () => {
    viewStore.dispatch({ type: 'setChannelLayout', channelLayout: channelLayoutSelect.value as ChannelLayout });
    loop?.invalidateWaveform();
  });
  amplitudeScaleSelect.addEventListener('change', () => {
    viewStore.dispatch({
      type: 'setAmplitudeScale',
      amplitudeScale: { type: amplitudeScaleSelect.value as AmplitudeScaleType, param: 2 },
    });
    loop?.invalidateWaveform();
  });
  followModeSelect.addEventListener('change', () => {
    viewStore.dispatch({ type: 'setFollowMode', followPlayhead: followModeSelect.value as FollowMode });
  });
  themeSelect.addEventListener('change', () => {
    const override = themeSelect.value as ThemeOverride;
    setThemeOverride(override);
    applyThemeOverride(override);
    loop?.refreshTheme();
  });

  colorMapSelect.addEventListener('change', () => {
    viewStore.dispatch({ type: 'setColorMap', colorMap: colorMapSelect.value as SpectrogramColorMap });
    loop?.invalidateSpectrogram(); // shader-only — instant, no tile regeneration (M07)
  });
  const applySpectrogramRange = (): void => {
    viewStore.dispatch({
      type: 'setSpectrogramRange',
      displayFloorDb: Number(floorDbInput.value),
      gainDb: Number(gainDbInput.value),
      gamma: Number(gammaInput.value),
    });
    loop?.invalidateSpectrogram(); // shader-only — instant, no tile regeneration (M07)
  };
  floorDbInput.addEventListener('input', applySpectrogramRange);
  gainDbInput.addEventListener('input', applySpectrogramRange);
  gammaInput.addEventListener('input', applySpectrogramRange);
  freqAxisSelect.addEventListener('change', () => {
    viewStore.dispatch({ type: 'setFreqAxis', freqAxis: freqAxisSelect.value as FreqAxis });
    loop?.invalidateSpectrogram(); // regenerates tiles (tile-config field) — old ones stay visible meanwhile
  });
  decimationSelect.addEventListener('change', () => {
    viewStore.dispatch({ type: 'setDecimation', decimation: decimationSelect.value as SpectrogramDecimationMode });
    loop?.invalidateSpectrogram();
  });

  function formatSeconds(seconds: number): string {
    const m = Math.floor(seconds / 60);
    const s = Math.floor(seconds % 60);
    return `${m}:${s.toString().padStart(2, '0')}`;
  }

  function render(state: TransportObservableState): void {
    const duration = state.durationSeconds ?? 0;
    if (!seeking) {
      seekSlider.max = String(Math.max(duration, 0.001));
      seekSlider.value = String(state.positionSeconds);
    }
    positionReadout.textContent = `${formatSeconds(state.positionSeconds)} / ${formatSeconds(duration)}${
      state.dropoutCount > 0 ? ` (${state.dropoutCount} dropouts)` : ''
    }`;
    playBtn.disabled = state.status === 'playing' || state.status === 'idle' || state.status === 'loading';
    pauseBtn.disabled = state.status !== 'playing';
  }

  fileInput.addEventListener('change', () => {
    void (async () => {
      const file = fileInput.files?.[0];
      if (!file) return;

      client?.dispose();
      client = null;
      latestTransportState = null;
      waveform?.dispose();
      waveform = null;
      session = null;
      selection = null;
      viewStore.dispatch({ type: 'panToStart', startFrame: 0 });
      playbackStatus.textContent = 'Decoding…';

      // M15: metadata parsing wants the *entire* file, unlike the decoder's probe slice below — a
      // trailing ID3v1 tag, an oversized APIC block, or MP4 atoms placed after `mdat` can all live
      // past whatever a probe would see. Read once, up front, independent of the decode pipeline.
      metadataSection.hidden = false;
      const fileBytes = new Uint8Array(await file.arrayBuffer());
      const metadata = engine.createMetadata(fileBytes);
      if (metadata) {
        try {
          renderMetadataPanel(metadataContent, { getPictureBytes: (i) => metadata.getPictureBytes(i) }, metadata.result);
        } finally {
          metadata.dispose();  // picture bytes are already copied out by renderMetadataPanel by now
        }
      } else {
        renderNoMetadata(metadataContent, 'Could not read this file for metadata.');
      }

      // 4MB rather than a smaller "should be plenty" probe: real-world MP3s with an ID3v2 tag
      // carrying embedded (especially high-resolution) cover art can have that tag alone run into
      // several MB, and the sniffer needs to see past the *entire* tag in one probe or it
      // correctly refuses to guess rather than risk a false-positive match on the tag's own binary
      // image data (see format_sniffer.cpp).
      const probeSlice = new Uint8Array(await file.slice(0, 4 * 1024 * 1024).arrayBuffer());
      const newSession = engine.createDecodeSession(probeSlice);
      if (!newSession) {
        playbackStatus.textContent = 'Unrecognised or unsupported file format (or its ID3v2 tag is larger than the 4MB probe).';
        return;
      }
      session = newSession;

      // Drives the waveform generator alongside decode (M04 "Streaming generation"): the
      // AudioBuffer only exists once the first frames land, so the Waveform handle is created
      // lazily on whichever step first sees a non-zero audioBufferHandle. Returns the
      // (possibly newly-created) handle rather than assigning the outer `waveform` from inside
      // this closure — TS can't narrow a `let` after a nested-function assignment, so the
      // reassignment happens at each call site instead, in the enclosing scope.
      const stepWaveform = (current: Waveform | null): Waveform | null => {
        const handle = current ? null : newSession.audioBufferHandle;
        const next = current ?? (handle ? engine.createWaveform(handle) : null);
        if (next) {
          next.processAvailableChunks();
          viewStore.setLimits({ totalFrames: Math.max(newSession.decodedFrameCount, 0), minFramesPerPixel: kMinFramesPerPixel });
          loop?.invalidateWaveform();
          updateAccessibleSummary();
        }
        return next;
      };

      newSession.feed(probeSlice);
      waveform = stepWaveform(waveform);
      let offset = probeSlice.length;
      while (offset < file.size) {
        const slice = new Uint8Array(await file.slice(offset, offset + 256 * 1024).arrayBuffer());
        newSession.feed(slice);
        offset += slice.length;
        waveform = stepWaveform(waveform);
      }
      newSession.finish();
      waveform?.finish();
      waveform = stepWaveform(waveform);

      const info = newSession.streamInfo;
      sourceSampleRate = info.sampleRate;
      const audioContext = new AudioContext();

      client = await TransportClient.create({
        audioContext,
        engine,
        decodeSession: newSession,
        sourceSampleRate: info.sampleRate,
        channelCount: Math.max(1, Math.min(2, info.channels)), // demo UI renders up to stereo
        workletModuleUrl: new URL('./playback/worklet/audProcessor.ts', import.meta.url),
      });
      client.setSourceComplete(true);
      // info.frameCount is -1 for codecs that don't (yet) report an exact/estimated count from
      // container metadata (e.g. MP3 without Xing/LAME header parsing — a known M02 gap, see
      // Mp3Decoder::info()). Since this demo decodes the whole file synchronously before this
      // point (session.finish() already ran), session.decodedFrameCount is the exact total
      // regardless of what the codec's own metadata could report — no need to wait on that gap.
      const durationFrames = info.frameCount >= 0 ? info.frameCount : newSession.decodedFrameCount;
      viewStore.setLimits({ totalFrames: Math.max(durationFrames, 0), minFramesPerPixel: kMinFramesPerPixel });
      viewStore.dispatch({ type: 'zoomToRange', startFrame: 0, endFrame: Math.max(durationFrames, 1) });
      client.load(durationFrames > 0 ? durationFrames : null);

      // M07: hand the spectrogram worker its own copy of the decoded PCM (one copy, not a
      // transfer — this thread still needs it for waveform queries). lastAppliedFftSize/
      // lastAppliedConfigKey reset so syncSpectrogramConfig() re-sends setConfig for the new track
      // rather than thinking the previous track's config still applies.
      if (durationFrames > 0) {
        const channels: Float32Array[] = [];
        for (let ch = 0; ch < info.channels; ch++) {
          channels.push(newSession.readChannelPcm(ch, 0, durationFrames));
        }
        lastAppliedFftSize = null;
        lastAppliedConfigKey = '';
        tileManager.loadPcm(channels, info.sampleRate);
      }

      client.subscribe((state) => {
        latestTransportState = state;
      });
      playbackStatus.textContent = `Loaded: ${info.codecName}, ${info.sampleRate}Hz, ${info.channels}ch`;
      playBtn.disabled = false;
      seekSlider.disabled = false;
      updateAccessibleSummary();
    })();
  });

  playBtn.addEventListener('click', () => {
    void client?.play();
  });

  pauseBtn.addEventListener('click', () => {
    client?.pause();
  });

  seekSlider.addEventListener('input', () => {
    seeking = true;
  });
  seekSlider.addEventListener('change', () => {
    client?.seekToSeconds(Number(seekSlider.value));
    seeking = false;
  });

  gainSlider.addEventListener('input', () => {
    client?.setGain(Number(gainSlider.value));
  });

  loopToggle.addEventListener('change', () => {
    if (!client) return;
    const duration = Number(seekSlider.max);
    client.setLoop(loopToggle.checked, 0, duration);
  });
}

void main();
