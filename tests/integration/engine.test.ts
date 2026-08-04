// Exercises the real built .wasm module under Node — not a mock — per M01's decision that native
// and Node suites must diverge as little as possible. The shipped wasm-release bundle targets
// -sENVIRONMENT=web,worker only, so this uses the wasm-test preset instead, which adds node to
// the environment list (see AUD_WASM_FOR_NODE in bindings/wasm/CMakeLists.txt). Expects
// `cmake --build --preset wasm-test` to have already produced
// build/wasm-test/bindings/wasm/aud_wasm.js.

import { beforeAll, describe, expect, it } from 'vitest';

// @ts-expect-error — built artifact, not present until the wasm-test preset has been built.
import createAudModule from '../../build/wasm-test/bindings/wasm/aud_wasm.js';

describe('aud_wasm Embind surface', () => {
  // Embind's type registry is scoped to the generated JS module, not to a Module instance —
  // calling createAudModule() more than once per process re-registers the same types and
  // throws "Cannot register type twice". Instantiate once and share across tests.
  let module: Awaited<ReturnType<typeof createAudModule>>;

  beforeAll(async () => {
    module = await createAudModule();
  });

  it('reports a build info object and passes the engine self-test', () => {
    const version = module.engineVersion();
    expect(version).toMatch(/^\d+\.\d+\.\d+$/);

    const buildInfo = module.engineBuildInfo();
    expect(buildInfo).toHaveProperty('simd');
    expect(buildInfo).toHaveProperty('threads');

    const selfTest = module.selfTest();
    expect(selfTest.pass).toBe(true);
  });

  it('decodes a minimal in-memory WAV via DecodeSession', () => {
    // A tiny valid WAV: RIFF/WAVE, fmt chunk (PCM, mono, 8kHz, 16-bit), data chunk with 4 silent frames.
    const bytes = buildMinimalWav();
    const ptr = module._malloc(bytes.length);
    module.HEAPU8.set(bytes, ptr);

    const session = module.DecodeSession.create(ptr, bytes.length);
    module._free(ptr);
    expect(session).not.toBeNull();

    const ptr2 = module._malloc(bytes.length);
    module.HEAPU8.set(bytes, ptr2);
    const feedResult = session.feedBytes(ptr2, bytes.length);
    module._free(ptr2);
    expect(feedResult.ok).toBe(true);

    const finishResult = session.finish();
    expect(finishResult.ok).toBe(true);

    const info = session.getStreamInfo();
    expect(info.sampleRate).toBe(8000);
    expect(info.channels).toBe(1);
    expect(session.getDecodedFrameCount()).toBe(4);

    session.delete();
  });
});

function buildMinimalWav(): Uint8Array {
  const frames = 4;
  const dataBytes = frames * 2; // mono, 16-bit
  const buffer = new ArrayBuffer(44 + dataBytes);
  const view = new DataView(buffer);
  let offset = 0;

  const writeString = (s: string) => {
    for (const ch of s) view.setUint8(offset++, ch.charCodeAt(0));
  };

  writeString('RIFF');
  view.setUint32(offset, 36 + dataBytes, true); offset += 4;
  writeString('WAVE');
  writeString('fmt ');
  view.setUint32(offset, 16, true); offset += 4; // fmt chunk size
  view.setUint16(offset, 1, true); offset += 2; // PCM
  view.setUint16(offset, 1, true); offset += 2; // mono
  view.setUint32(offset, 8000, true); offset += 4; // sample rate
  view.setUint32(offset, 16000, true); offset += 4; // byte rate
  view.setUint16(offset, 2, true); offset += 2; // block align
  view.setUint16(offset, 16, true); offset += 2; // bits per sample
  writeString('data');
  view.setUint32(offset, dataBytes, true); offset += 4;
  // 4 silent 16-bit samples
  for (let i = 0; i < frames; i += 1) {
    view.setInt16(offset, 0, true);
    offset += 2;
  }

  return new Uint8Array(buffer);
}
