# audWASM

Browser-based audio analysis & visualisation engine. C++20 core → WebAssembly, TypeScript frontend,
zero server-side processing. See `documentation/tasks/00-INDEX.md` for the full milestone plan; this
README covers only "how do I build and run it."

## Prerequisites

- CMake >= 3.24, Ninja
- A native C++20 compiler: Clang 16+, GCC 12+, or MSVC 19.36+
- [Emscripten](https://emscripten.org/) pinned to the version in `.emscripten-version` (via `emsdk`)
- Node.js >= 20 (frontend, integration tests, scripts)
- `ffmpeg` locally if you need to (re)generate test fixtures (`scripts/make-fixtures.mjs`) — not
  required for building or running the existing committed fixtures

## Building

```sh
# Native (Debug, with ASan/UBSan on Linux/macOS)
cmake --preset native-debug
cmake --build --preset native-debug
ctest --preset native-debug

# Native (Release, for benchmarking)
cmake --preset native-release
cmake --build --preset native-release

# WASM (Release)
source /path/to/emsdk/emsdk_env.sh
cmake --preset wasm-release
cmake --build --preset wasm-release
```

This produces `build/wasm-release/bindings/wasm/aud_wasm.{js,wasm}`.

## Running the CLI

```sh
build/native-debug/tools/cli/aud_cli --version
build/native-debug/tools/cli/aud_cli --self-test
build/native-debug/tools/cli/aud_cli decode path/to/file.wav
```

## Running the frontend

```sh
cd frontend
npm install
npm run dev
```

Opens a Vite dev server that instantiates the engine and shows build info + a self-test result.
`npm run build` produces a static bundle deployable to any static host — no COOP/COEP headers
required, since v1 is single-threaded WASM (see `documentation/tasks/M00-foundations-and-conventions.md` §5).

## Running the test suites

| Suite | Command |
|---|---|
| C++ unit tests (Catch2) | `ctest --preset native-debug` (after building) |
| Node integration tests (real `.wasm` under Node) | `cd tests/integration && npm ci && npm test` (after building `wasm-release`) |
| Frontend unit tests (Vitest) | `cd frontend && npm test` |
| Frontend type-check | `cd frontend && npx tsc --noEmit` |

## Repository layout

See `documentation/tasks/M00-foundations-and-conventions.md` § "Repository layout" for the full
annotated tree.

## Status

Bootstrapped: M00 (foundations), M01 (infrastructure), M02 (audio decoding — WAV/FLAC/MP3/Ogg
Vorbis via vendored dr_libs/stb_vorbis; AAC/M4A via the browser `decodeAudioData()` fallback).
Everything downstream (playback, waveform, FFT, analysis, etc.) is unimplemented — see
`documentation/tasks/00-INDEX.md` for the full milestone list and suggested delivery phases.

Known gaps from this initial pass (tracked as follow-ups, not silently skipped):

- MP3 Xing/LAME/VBRI header parsing (frame count, encoder delay/padding) is not implemented yet —
  `Mp3Decoder::info()` currently always reports an estimate with no delay/padding. See the
  `TODO(M02)` in `engine/decoder/mp3_decoder.cpp`.
- AIFF is a stretch target per M02 and is not implemented (`createDecoder` returns
  `UnsupportedFormat` for it).
- The fuzz target, the >2-channel decision, and the full multi-OS CI run have not been exercised in
  this environment (no `cmake`/`emsdk` available where this was bootstrapped) — see
  `documentation/tasks/M00-foundations-and-conventions.md`'s open question and
  `documentation/tasks/M02-audio-decoding.md`'s fuzz target task.
