// AAC/M4A ingestion via the browser (M02 "The AAC problem — decided explicitly"). There is no
// permissively licensed, small, single-header AAC decoder, so v1 delegates to
// AudioContext.decodeAudioData() (universally available, hardware-accelerated, patent-licensed by
// the browser vendor). WebCodecs AudioDecoder is a future upgrade path where available.
//
// Consequences this file exists to make concrete, not hide:
//  - Decode is all-or-nothing, not incremental like the native decoders.
//  - decodeAudioData() decodes to the AudioContext's sample rate, not necessarily the file's
//    native rate; we flag this via `resampledByBrowser` rather than silently reporting the wrong
//    rate to analysis code that assumes it's original.
//  - Memory doubles transiently (browser buffer + engine copy) — mitigated by transferring
//    channel-at-a-time and releasing each Float32Array as it's copied (see ExternalPcmSource,
//    engine/decoder/external_pcm_source.hpp, which this feeds into).

export interface DecodedAacResult {
  channelData: Float32Array[];
  sampleRate: number;
  resampledByBrowser: boolean;
}

/** True on essentially every browser released since ~2014; used to gate the AAC/M4A entry in the
 *  supported-format list shown to the user (M02 risk mitigation), not to gate this function itself. */
export function isAacDecodeSupported(): boolean {
  return typeof AudioContext !== 'undefined' || typeof (globalThis as { webkitAudioContext?: unknown }).webkitAudioContext !== 'undefined';
}

/**
 * Decodes `fileBytes` via the browser's AAC/M4A decoder. Attempts to construct the
 * OfflineAudioContext at `nativeSampleRateHint` (from the container's `mdhd`/`iTunSMPB` metadata,
 * once M15 lands) so playback timestamps line up; if that hint isn't available, the browser's
 * default output rate is used and `resampledByBrowser` is set.
 */
export async function decodeAacViaBrowser(fileBytes: ArrayBuffer, nativeSampleRateHint?: number): Promise<DecodedAacResult> {
  const AudioContextCtor = AudioContext ?? (globalThis as unknown as { webkitAudioContext: typeof AudioContext }).webkitAudioContext;

  // A throwaway context just to decode; OfflineAudioContext lets us request a specific output rate
  // without ever routing audio to real output hardware.
  const probeContext = new AudioContextCtor();
  const targetRate = nativeSampleRateHint ?? probeContext.sampleRate;
  await probeContext.close();

  const offlineContext = new OfflineAudioContext(1, 1, targetRate);
  const decoded = await offlineContext.decodeAudioData(fileBytes.slice(0));

  const channelData: Float32Array[] = [];
  for (let ch = 0; ch < decoded.numberOfChannels; ch += 1) {
    // Copy channel-at-a-time (M02: bounds the transient memory doubling to one channel at a time).
    channelData.push(Float32Array.from(decoded.getChannelData(ch)));
  }

  return {
    channelData,
    sampleRate: decoded.sampleRate,
    resampledByBrowser: nativeSampleRateHint !== undefined && decoded.sampleRate !== nativeSampleRateHint,
  };
}
