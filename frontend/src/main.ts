import { AudioEngine, type DecodeSession, type Waveform } from '../../bindings/wasm/engine.ts';
import { TransportClient, type TransportObservableState } from './playback/transportClient.ts';

// A visible frame range, in source-track frames. `null` (used by callers, not stored here) means
// "the whole decoded track so far" — auto-following progressive decode until the user zooms/pans.
interface FrameRange {
  start: number;
  end: number;
}

// Minimal Canvas2D waveform draw (the real renderer is M17) — just enough to prove the M05
// mipmap-pyramid query() is correct at every zoom level and to demonstrate progressive rendering:
// called again after every feed() so bins appear on screen as chunks complete rather than only
// once decode finishes.
function drawWaveform(canvas: HTMLCanvasElement, waveform: Waveform, range: FrameRange): void {
  const frames = range.end - range.start;
  if (frames <= 0) return;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const channels = Math.max(1, Math.min(2, waveform.channelCount));
  const bandHeight = canvas.height / channels;
  const binCount = canvas.width;
  const { bins, isRawPcm } = waveform.query(0 /* PerChannel */, range.start, range.end, binCount);

  // M05 "Below level 0": once zoomed in past the pyramid's finest bin, the engine reduces raw PCM
  // per pixel instead of aggregating mipmap bins — tint the trace to make the crossover visible.
  ctx.fillStyle = isRawPcm ? '#ffb85f' : '#5fd3ff';
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

// Keeps `range` within [0, totalFrames) and never lets a zoom collapse below a handful of frames
// (a zero-width query is meaningless, and a single-frame one degenerates the raw-PCM fallback).
function clampRange(start: number, end: number, totalFrames: number): FrameRange {
  const kMinSpanFrames = 32;
  const span = Math.min(Math.max(kMinSpanFrames, end - start), Math.max(totalFrames, kMinSpanFrames));
  const clampedStart = Math.min(Math.max(start, 0), Math.max(totalFrames - span, 0));
  return { start: clampedStart, end: clampedStart + span };
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
  let session: DecodeSession | null = null;
  let seeking = false;

  // null = auto-follow the whole decoded-so-far track; set once the user zooms or pans (M05 demo:
  // "zoom/pan interaction proving 60 fps").
  let viewRange: FrameRange | null = null;

  function redrawWaveform(): void {
    if (!waveform || !session) return;
    const totalFrames = Math.max(session.decodedFrameCount, 0);
    const range = viewRange ?? { start: 0, end: totalFrames };
    drawWaveform(waveformCanvas, waveform, range);
  }

  // Wheel-zoom around the cursor's frame position; drag-to-pan. Both just narrow/shift the frame
  // range passed to query() — the pyramid picks whatever level fits, so this stays smooth (O(1) per
  // frame) all the way from whole-track down to single-sample, per M05's acceptance criterion.
  waveformCanvas.addEventListener('wheel', (event) => {
    if (!session) return;
    event.preventDefault();
    const totalFrames = Math.max(session.decodedFrameCount, 0);
    const current = viewRange ?? { start: 0, end: totalFrames };
    const span = current.end - current.start;

    const rect = waveformCanvas.getBoundingClientRect();
    const fractionAcross = rect.width > 0 ? (event.clientX - rect.left) / rect.width : 0.5;
    const pivotFrame = current.start + fractionAcross * span;

    const zoomFactor = event.deltaY > 0 ? 1.25 : 1 / 1.25;
    const newSpan = span * zoomFactor;
    const newStart = pivotFrame - fractionAcross * newSpan;
    viewRange = clampRange(newStart, newStart + newSpan, totalFrames);
    redrawWaveform();
  });

  let dragOriginX = 0;
  let dragOriginRange: FrameRange | null = null;
  waveformCanvas.addEventListener('pointerdown', (event) => {
    if (!session) return;
    dragOriginX = event.clientX;
    const totalFrames = Math.max(session.decodedFrameCount, 0);
    dragOriginRange = viewRange ?? { start: 0, end: totalFrames };
    waveformCanvas.setPointerCapture(event.pointerId);
  });
  waveformCanvas.addEventListener('pointermove', (event) => {
    if (!dragOriginRange || !session) return;
    const totalFrames = Math.max(session.decodedFrameCount, 0);
    const rect = waveformCanvas.getBoundingClientRect();
    const span = dragOriginRange.end - dragOriginRange.start;
    const framesPerPixel = rect.width > 0 ? span / rect.width : 0;
    const deltaFrames = (event.clientX - dragOriginX) * framesPerPixel;
    viewRange = clampRange(dragOriginRange.start - deltaFrames, dragOriginRange.end - deltaFrames, totalFrames);
    redrawWaveform();
  });
  waveformCanvas.addEventListener('pointerup', (event) => {
    dragOriginRange = null;
    waveformCanvas.releasePointerCapture(event.pointerId);
  });
  waveformCanvas.addEventListener('dblclick', () => {
    viewRange = null;  // reset to whole-track view
    redrawWaveform();
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
      waveform?.dispose();
      waveform = null;
      session = null;
      viewRange = null;
      playbackStatus.textContent = 'Decoding…';

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
      // lazily on whichever step first sees a non-zero audioBufferHandle.
      const stepWaveform = (): void => {
        if (!waveform) {
          const handle = newSession.audioBufferHandle;
          if (handle) waveform = engine.createWaveform(handle);
        }
        if (waveform) {
          waveform.processAvailableChunks();
          redrawWaveform();
        }
      };

      newSession.feed(probeSlice);
      stepWaveform();
      let offset = probeSlice.length;
      while (offset < file.size) {
        const slice = new Uint8Array(await file.slice(offset, offset + 256 * 1024).arrayBuffer());
        newSession.feed(slice);
        offset += slice.length;
        stepWaveform();
      }
      newSession.finish();
      waveform?.finish();
      stepWaveform();

      const info = newSession.streamInfo;
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
