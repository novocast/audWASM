// Decode runs in a Web Worker from the start (M02 decision): even a fast MP3 decode of a
// 10-minute track is hundreds of milliseconds, which on the main thread means dropped frames at
// exactly the moment the user is watching the waveform draw in.
//
// This worker owns one module instance and (per the M02 open question, proposal (a)) keeps the
// decoded PCM worker-side — the main thread gets progress/summary messages, not the raw buffer.
// Playback (M03) will feed its ring buffer from here directly.

import { AudioEngine } from '../../bindings/wasm/engine.ts';
import type { StreamInfo } from '../../bindings/wasm/engine.d.ts';

interface StartMessage {
  type: 'start';
  file: Blob;
}

type InboundMessage = StartMessage;

interface ProgressMessage {
  type: 'progress';
  framesDecoded: number;
  estimatedTotalFrames: number;
  seconds: number;
}

interface DoneMessage {
  type: 'done';
  streamInfo: StreamInfo;
}

interface ErrorMessage {
  type: 'error';
  message: string;
}

type OutboundMessage = ProgressMessage | DoneMessage | ErrorMessage;

const READ_SLICE_BYTES = 256 * 1024; // M02: JS reads 256KB slices

let enginePromise: Promise<AudioEngine> | null = null;

function getEngine(): Promise<AudioEngine> {
  enginePromise ??= AudioEngine.create();
  return enginePromise;
}

function post(message: OutboundMessage): void {
  postMessage(message);
}

async function decodeFile(file: Blob): Promise<void> {
  const engine = await getEngine();

  const firstSlice = new Uint8Array(await file.slice(0, READ_SLICE_BYTES).arrayBuffer());
  const session = engine.createDecodeSession(firstSlice);
  if (!session) {
    post({ type: 'error', message: 'unrecognised or unsupported file format' });
    return;
  }

  let offset = READ_SLICE_BYTES;
  session.feed(firstSlice);

  while (offset < file.size) {
    const slice = new Uint8Array(await file.slice(offset, offset + READ_SLICE_BYTES).arrayBuffer());
    session.feed(slice);
    offset += READ_SLICE_BYTES;

    post({
      type: 'progress',
      framesDecoded: session.decodedFrameCount,
      estimatedTotalFrames: session.streamInfo.frameCount,
      seconds: 0,
    });
  }

  session.finish();
  post({ type: 'done', streamInfo: session.streamInfo });
}

self.onmessage = (event: MessageEvent<InboundMessage>) => {
  const message = event.data;
  if (message.type === 'start') {
    decodeFile(message.file).catch((err: unknown) => {
      post({ type: 'error', message: err instanceof Error ? err.message : String(err) });
    });
  }
};
