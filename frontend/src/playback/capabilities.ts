// Capability detection for the playback engine. See M03 "Architecture": SharedArrayBuffer is
// strictly an enhancement (better latency/jitter, exact position) gated on `crossOriginIsolated`;
// the app must work fully without it (M01's "no COOP/COEP required" acceptance criterion).

export interface PlaybackCapabilities {
  /** Gates the SAB ring path (sabSink.ts). False on any page not serving COOP/COEP headers. */
  readonly sharedArrayBuffer: boolean;
  /** AudioWorklet is supported everywhere we care about (M03 design), but we still check rather
   *  than assume, since the AudioBufferSourceNode fallback exists precisely for its absence. */
  readonly audioWorklet: boolean;
  /** `AudioContext.outputLatency` is missing on Safari (M03 risk table); `baseLatency` is the
   *  fallback used for the same position-correction purpose when this is false. */
  readonly outputLatency: boolean;
}

export function detectCapabilities(): PlaybackCapabilities {
  const hasSharedArrayBuffer = typeof SharedArrayBuffer !== 'undefined';
  const isCrossOriginIsolated =
    typeof crossOriginIsolated !== 'undefined' ? crossOriginIsolated : false;

  const AudioContextCtor =
    typeof AudioContext !== 'undefined'
      ? AudioContext
      : ((globalThis as { webkitAudioContext?: typeof AudioContext }).webkitAudioContext ?? undefined);

  return {
    sharedArrayBuffer: hasSharedArrayBuffer && isCrossOriginIsolated,
    audioWorklet: typeof AudioContextCtor !== 'undefined' && 'audioWorklet' in AudioContextCtor.prototype,
    outputLatency: 'outputLatency' in (AudioContextCtor?.prototype ?? {}),
  };
}
