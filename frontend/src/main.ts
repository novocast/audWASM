import { AudioEngine, type Waveform } from '../../bindings/wasm/engine.ts';
import { TransportClient, type TransportObservableState } from './playback/transportClient.ts';

// Minimal Canvas2D waveform draw (the real renderer is M17) — just enough to prove the M04 data
// is correct and to demonstrate progressive rendering: called again after every feed() so bins
// appear on screen as chunks complete rather than only once decode finishes.
function drawWaveform(canvas: HTMLCanvasElement, waveform: Waveform, decodedFrames: number): void {
  if (decodedFrames <= 0) return;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const channels = Math.max(1, Math.min(2, waveform.channelCount));
  const bandHeight = canvas.height / channels;
  const binCount = canvas.width;
  const { bins } = waveform.query(0 /* PerChannel */, 0, decodedFrames, binCount);

  ctx.fillStyle = '#5fd3ff';
  for (let ch = 0; ch < channels; ch++) {
    const midY = bandHeight * ch + bandHeight / 2;
    const scale = bandHeight / 2;
    for (let i = 0; i < binCount; i++) {
      const idx = ch * binCount + i;
      const y1 = midY - bins.max(idx) * scale;
      const y2 = midY - bins.min(idx) * scale;
      ctx.fillRect(i, Math.min(y1, y2), 1, Math.max(1, Math.abs(y2 - y1)));
    }
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
  const waveformCanvas = document.querySelector<HTMLCanvasElement>('#waveform-canvas')!;

  section.hidden = false;

  let client: TransportClient | null = null;
  let waveform: Waveform | null = null;
  let seeking = false;

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
      waveform?.dispose();
      waveform = null;
      playbackStatus.textContent = 'Decoding…';

      // 4MB rather than a smaller "should be plenty" probe: real-world MP3s with an ID3v2 tag
      // carrying embedded (especially high-resolution) cover art can have that tag alone run into
      // several MB, and the sniffer needs to see past the *entire* tag in one probe or it
      // correctly refuses to guess rather than risk a false-positive match on the tag's own binary
      // image data (see format_sniffer.cpp).
      const probeSlice = new Uint8Array(await file.slice(0, 4 * 1024 * 1024).arrayBuffer());
      const session = engine.createDecodeSession(probeSlice);
      if (!session) {
        playbackStatus.textContent = 'Unrecognised or unsupported file format (or its ID3v2 tag is larger than the 4MB probe).';
        return;
      }

      // Drives the waveform generator alongside decode (M04 "Streaming generation"): the
      // AudioBuffer only exists once the first frames land, so the Waveform handle is created
      // lazily on whichever step first sees a non-zero audioBufferHandle.
      const stepWaveform = (): void => {
        if (!waveform) {
          const handle = session.audioBufferHandle;
          if (handle) waveform = engine.createWaveform(handle);
        }
        if (waveform) {
          waveform.processAvailableChunks();
          drawWaveform(waveformCanvas, waveform, session.decodedFrameCount);
        }
      };

      session.feed(probeSlice);
      stepWaveform();
      let offset = probeSlice.length;
      while (offset < file.size) {
        const slice = new Uint8Array(await file.slice(offset, offset + 256 * 1024).arrayBuffer());
        session.feed(slice);
        offset += slice.length;
        stepWaveform();
      }
      session.finish();
      waveform?.finish();
      stepWaveform();

      const info = session.streamInfo;
      const audioContext = new AudioContext();

      client = await TransportClient.create({
        audioContext,
        engine,
        decodeSession: session,
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
      const durationFrames = info.frameCount >= 0 ? info.frameCount : session.decodedFrameCount;
      client.load(durationFrames > 0 ? durationFrames : null);

      client.subscribe(render);
      playbackStatus.textContent = `Loaded: ${info.codecName}, ${info.sampleRate}Hz, ${info.channels}ch`;
      playBtn.disabled = false;
      seekSlider.disabled = false;

      const tick = (nowMs: number): void => {
        if (!client) return;
        render(client.tick(nowMs));
        requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
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
