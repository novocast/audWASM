// postMessage-based fallback PcmSink, used when `crossOriginIsolated` is false (the default
// deployment, per M01 — the app must work fully without COOP/COEP). Per M03 "Architecture": the
// worklet keeps an internal queue fed by `port.postMessage` with transferred Float32Arrays,
// prebuffered to `capacityFrames` worth of audio. Backpressure is a simple frame-credit scheme:
// the writer tracks how many frames it has sent but not yet been told were consumed, and refuses
// to exceed `capacityFrames` in flight; the reader reports consumption back in small batches
// rather than per-render-quantum, to keep message traffic low.

import type { PcmSinkReader, PcmSinkWriter } from './pcmSink.ts';

interface PcmChunkMessage {
  type: 'pcmChunk';
  planar: Float32Array[];
  frameCount: number;
}

interface PcmConsumedMessage {
  type: 'pcmConsumed';
  frames: number;
}

type PostMessageSinkMessage = PcmChunkMessage | PcmConsumedMessage;

function isSinkMessage(data: unknown): data is PostMessageSinkMessage {
  return typeof data === 'object' && data !== null && ((data as { type?: unknown }).type === 'pcmChunk' || (data as { type?: unknown }).type === 'pcmConsumed');
}

export class PostMessageSinkWriter implements PcmSinkWriter {
  private framesInFlight = 0;

  constructor(
    private readonly port: MessagePort,
    public readonly channelCount: number,
    public readonly capacityFrames: number,
  ) {
    // addEventListener rather than `port.onmessage =`, which is a single-slot property that would
    // clobber any other listener (e.g. TransportClient's own dropout-count listener) attached to
    // the same port. MessagePort requires an explicit start() when only addEventListener is used.
    this.port.addEventListener('message', (event: MessageEvent) => {
      if (isSinkMessage(event.data) && event.data.type === 'pcmConsumed') {
        this.framesInFlight = Math.max(0, this.framesInFlight - event.data.frames);
      }
    });
    this.port.start();
  }

  framesAvailableToWrite(): number {
    return Math.max(0, this.capacityFrames - this.framesInFlight);
  }

  write(planar: Float32Array[], frameCount: number): number {
    const toWrite = Math.min(frameCount, this.framesAvailableToWrite());
    if (toWrite === 0) {
      return 0;
    }
    const planarCopy: Float32Array[] = [];
    for (let ch = 0; ch < this.channelCount; ch++) {
      planarCopy.push(planar[ch]!.slice(0, toWrite));
    }
    this.port.postMessage(
      { type: 'pcmChunk', planar: planarCopy, frameCount: toWrite } satisfies PcmChunkMessage,
      planarCopy.map((a) => a.buffer),
    );
    this.framesInFlight += toWrite;
    return toWrite;
  }

  reset(): void {
    this.framesInFlight = 0;
    this.port.postMessage({ type: 'pcmConsumed', frames: 0 } satisfies PcmConsumedMessage); // no-op keepalive; queue clearing is a separate control message (see transportClient)
  }
}

export class PostMessageSinkReader implements PcmSinkReader {
  private readonly queue: Array<{ planar: Float32Array[]; offset: number; length: number }> = [];
  private totalQueued = 0;
  private consumedSinceReport = 0;

  constructor(
    private readonly port: MessagePort,
    public readonly channelCount: number,
  ) {
    this.port.addEventListener('message', (event: MessageEvent) => {
      if (isSinkMessage(event.data) && event.data.type === 'pcmChunk') {
        this.queue.push({ planar: event.data.planar, offset: 0, length: event.data.frameCount });
        this.totalQueued += event.data.frameCount;
      }
    });
    this.port.start();
  }

  framesAvailableToRead(): number {
    return this.totalQueued;
  }

  read(planarOut: Float32Array[], frameCount: number): number {
    let written = 0;
    while (written < frameCount && this.queue.length > 0) {
      const front = this.queue[0]!;
      const available = front.length - front.offset;
      const take = Math.min(available, frameCount - written);
      for (let ch = 0; ch < this.channelCount; ch++) {
        planarOut[ch]!.set(front.planar[ch]!.subarray(front.offset, front.offset + take), written);
      }
      front.offset += take;
      written += take;
      this.totalQueued -= take;
      if (front.offset >= front.length) {
        this.queue.shift();
      }
    }

    this.consumedSinceReport += written;
    if (this.consumedSinceReport >= 256) {
      this.port.postMessage({ type: 'pcmConsumed', frames: this.consumedSinceReport } satisfies PcmConsumedMessage);
      this.consumedSinceReport = 0;
    }
    return written;
  }

  /** Drops all queued content (M03 "Seeking" step 2) and immediately reports it consumed so the
   *  writer's credit doesn't stay pinned against frames that will never be reported otherwise. */
  reset(): void {
    const dropped = this.totalQueued;
    this.queue.length = 0;
    this.totalQueued = 0;
    this.consumedSinceReport = 0;
    if (dropped > 0) {
      this.port.postMessage({ type: 'pcmConsumed', frames: dropped } satisfies PcmConsumedMessage);
    }
  }
}
