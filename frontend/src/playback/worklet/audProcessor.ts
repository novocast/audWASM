// The AudioWorkletProcessor: a deliberately dumb consumer. Gain and the seek/loop discontinuity
// fades are applied producer-side (see playback_bindings.cpp's TransportHandle::renderInto and
// engine/playback/transport.cpp's seek fade-in) — this processor's own responsibility is limited to
// (a) draining whichever PcmSink reader it was configured with, (b) silence-filling and counting a
// dropout on underrun, and (c) a short cosine fade-out of its own when the ring runs dry (covers
// pause and end-of-track, where the *producer* stops feeding but doesn't itself know to fade
// anything, since nothing discontinuous happened on its side — the discontinuity is "the ring
// emptied", which only the consumer can see).

import type { PcmSinkConfig, PcmSinkReader } from '../pcmSink.ts';
import { SabRingReader } from '../sabSink.ts';
import { PostMessageSinkReader } from '../postMessageSink.ts';

const kFadeOutFrames = 128; // one render quantum, ~2.7-2.9ms at 44.1k/48k — matches fade.hpp's default

interface DropoutMessage {
  type: 'dropout';
  missingFrames: number;
}

class AudProcessor extends AudioWorkletProcessor {
  private reader: PcmSinkReader | null = null;
  private readonly channelCount: number;
  private hadAudioLastQuantum = false;

  constructor(options?: AudioWorkletNodeOptions) {
    super(options);
    const config = options?.processorOptions as PcmSinkConfig | undefined;
    this.channelCount = config?.channelCount ?? 2;

    if (config?.mode === 'sab') {
      this.reader = new SabRingReader(config.buffer, config.channelCount, config.capacityFrames);
    } else if (config?.mode === 'postMessage') {
      this.reader = new PostMessageSinkReader(this.port, config.channelCount);
    }
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const output = outputs[0];
    if (!output || output.length === 0 || !this.reader) {
      return true;
    }

    const frameCount = output[0]!.length;
    const channels = Math.min(output.length, this.channelCount);
    const planarOut = output.slice(0, channels);

    const got = this.reader.read(planarOut, frameCount);

    if (got < frameCount) {
      for (let ch = 0; ch < channels; ch++) {
        output[ch]!.fill(0, got);
      }
      if (got < frameCount && this.hadAudioLastQuantum) {
        this.port.postMessage({ type: 'dropout', missingFrames: frameCount - got } satisfies DropoutMessage);
      }
    }

    // Fade out over the tail of whatever we did get, the render quantum the ring first ran dry
    // (covers pause and end-of-track discontinuities — see file header).
    if (got > 0 && got < frameCount) {
      const fadeFrames = Math.min(got, kFadeOutFrames);
      const start = got - fadeFrames;
      for (let i = 0; i < fadeFrames; i++) {
        const eased = 0.5 - 0.5 * Math.cos((i / Math.max(1, fadeFrames - 1)) * Math.PI);
        const gain = 1 - eased;
        for (let ch = 0; ch < channels; ch++) {
          output[ch]![start + i]! *= gain;
        }
      }
    }

    this.hadAudioLastQuantum = got > 0;
    return true;
  }
}

registerProcessor('aud-processor', AudProcessor);
