// Minimal AudioBufferSourceNode fallback for environments without AudioWorklet (M03: "Keep a
// minimal AudioBufferSourceNode fallback behind a capability check ... with reduced features").
//
// Documented feature reduction vs. the AudioWorklet path:
//   - No gapless seek: seeking stops the current source node and starts a new one, which is
//     audibly cheap but not the <10ms budget M22 sets for the worklet path, and is not click-free
//     (no fade — AudioBufferSourceNode gives no mid-playback hook to ramp before a stop/start).
//   - Coarser position: derived from `audioContext.currentTime` deltas rather than an atomic
//     frame counter, so it drifts slightly across pause/resume (the exact problem the M03 design
//     section on `AudioBufferSourceNode` calls out) and is not corrected for output latency.
//   - No progressive playback: the entire decoded buffer must be available before playback starts
//     (this path is only reached once decode has finished).
//   - Loop crossfade is not implemented; `loop`/`loopStart`/`loopEnd` are the browser's own exact
//     (click-risking) loop points.

export type AudioBufferFallbackStatus = 'idle' | 'playing' | 'paused' | 'ended';

export class AudioBufferFallbackPlayer {
  private source: AudioBufferSourceNode | null = null;
  private readonly gainNode: GainNode;
  private status: AudioBufferFallbackStatus = 'idle';
  private startedAtContextTime = 0; // audioContext.currentTime when playback last started/resumed
  private offsetSeconds = 0; // playback position at that start, in seconds

  constructor(
    private readonly audioContext: AudioContext,
    private readonly buffer: AudioBuffer,
  ) {
    this.gainNode = audioContext.createGain();
    this.gainNode.connect(audioContext.destination);
  }

  private makeSource(): AudioBufferSourceNode {
    const node = audioContext_createSource(this.audioContext, this.buffer);
    node.connect(this.gainNode);
    node.onended = () => {
      if (this.status === 'playing') {
        this.status = 'ended';
      }
    };
    return node;
  }

  play(): void {
    if (this.status === 'playing') return;
    this.source = this.makeSource();
    this.source.start(0, this.offsetSeconds);
    this.startedAtContextTime = this.audioContext.currentTime;
    this.status = 'playing';
  }

  pause(): void {
    if (this.status !== 'playing' || !this.source) return;
    this.offsetSeconds += this.audioContext.currentTime - this.startedAtContextTime;
    this.source.stop();
    this.source = null;
    this.status = 'paused';
  }

  seekTo(seconds: number): void {
    const wasPlaying = this.status === 'playing';
    if (this.source) {
      this.source.stop();
      this.source = null;
    }
    this.offsetSeconds = Math.max(0, Math.min(seconds, this.buffer.duration));
    if (wasPlaying) {
      this.source = this.makeSource();
      this.source.start(0, this.offsetSeconds);
      this.startedAtContextTime = this.audioContext.currentTime;
      this.status = 'playing';
    }
  }

  setLoop(enabled: boolean, startSeconds?: number, endSeconds?: number): void {
    if (this.source) {
      this.source.loop = enabled;
      if (startSeconds !== undefined) this.source.loopStart = startSeconds;
      if (endSeconds !== undefined) this.source.loopEnd = endSeconds;
    }
  }

  setGain(gain: number): void {
    this.gainNode.gain.value = gain;
  }

  get positionSeconds(): number {
    if (this.status === 'playing') {
      return this.offsetSeconds + (this.audioContext.currentTime - this.startedAtContextTime);
    }
    return this.offsetSeconds;
  }

  get currentStatus(): AudioBufferFallbackStatus {
    return this.status;
  }

  dispose(): void {
    this.source?.stop();
    this.source = null;
    this.gainNode.disconnect();
  }
}

function audioContext_createSource(ctx: AudioContext, buffer: AudioBuffer): AudioBufferSourceNode {
  const node = ctx.createBufferSource();
  node.buffer = buffer;
  return node;
}

/** Copies planar Float32 channel data (as produced by the decode engine) into a native
 *  browser AudioBuffer, for use with this fallback player. */
export function planarToAudioBuffer(
  audioContext: AudioContext,
  planar: Float32Array[],
  sampleRate: number,
): AudioBuffer {
  const frameCount = planar[0]?.length ?? 0;
  const buffer = audioContext.createBuffer(planar.length, frameCount, sampleRate);
  for (let ch = 0; ch < planar.length; ch++) {
    // Fresh copy guarantees a plain ArrayBuffer-backed view — copyToChannel() rejects a
    // SharedArrayBuffer-backed Float32Array, which `planar[ch]` could in principle be if the
    // caller sourced it from a SAB ring.
    buffer.copyToChannel(new Float32Array(planar[ch]!), ch);
  }
  return buffer;
}
