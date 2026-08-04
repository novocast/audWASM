// Common interface both ring implementations (sabSink.ts, postMessageSink.ts) satisfy, per M03
// "the two paths sit behind one PcmSink interface". The producer (main thread or worker, driving
// Transport.pump()/render()) only ever talks to a PcmSinkWriter; the AudioWorkletProcessor
// (audProcessor.ts) only ever talks to a PcmSinkReader. Neither side needs to know which transport
// mechanism (SharedArrayBuffer + Atomics, or postMessage + a prebuffered queue) is underneath.

export interface PcmSinkWriter {
  readonly channelCount: number;
  readonly capacityFrames: number;

  /** Writes up to `frameCount` frames from `planar` (one Float32Array per channel, each at least
   *  `frameCount` long). Returns frames actually written — may be less than requested if the sink
   *  is full; the caller must retry later rather than drop the remainder. */
  write(planar: Float32Array[], frameCount: number): number;

  framesAvailableToWrite(): number;

  /** Drops all buffered content. Only safe when the producer is quiesced (M03 "Seeking" step 2). */
  reset(): void;
}

export interface PcmSinkReader {
  readonly channelCount: number;

  /** Reads up to `frameCount` frames into `planarOut` (one Float32Array per channel, each at least
   *  `frameCount` long). Returns frames actually read — a shortfall is an underrun; the caller must
   *  silence-fill the remainder itself (see M03 risk table: "Worklet starvation under main-thread
   *  load"). */
  read(planarOut: Float32Array[], frameCount: number): number;

  framesAvailableToRead(): number;
}

/** What the worklet needs to construct the reader side of whichever sink mode is active. Sent as
 *  `AudioWorkletNodeOptions.processorOptions` at node-construction time (processorOptions is the
 *  only channel available before the port exists). */
export type PcmSinkConfig =
  | { mode: 'sab'; buffer: SharedArrayBuffer; channelCount: number; capacityFrames: number }
  | { mode: 'postMessage'; channelCount: number };
