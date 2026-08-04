// Ambient types for AudioWorkletGlobalScope. TypeScript's bundled `lib.dom.d.ts` does not include
// these (there is no "AudioWorklet" lib entry — see M03 survey notes), so this file supplies the
// minimal surface audProcessor.ts actually uses. Scoped to files that opt in by being under
// `playback/worklet/` and importing nothing that pulls in conflicting DOM globals.

declare const sampleRate: number;
declare const currentTime: number;
declare const currentFrame: number;

declare abstract class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: AudioWorkletNodeOptions);
  abstract process(
    inputs: Float32Array[][],
    outputs: Float32Array[][],
    parameters: Record<string, Float32Array>,
  ): boolean;
}

declare function registerProcessor(
  name: string,
  processorCtor: new (options?: AudioWorkletNodeOptions) => AudioWorkletProcessor,
): void;
