import { defineConfig } from 'vite';
import { fileURLToPath } from 'node:url';

export default defineConfig({
  // No COOP/COEP headers required (M01 acceptance criterion): the build must be servable from any
  // static host, which is only true as long as we never enable SharedArrayBuffer/pthreads (M00 §5).
  server: {
    fs: {
      // aud_wasm.js/.wasm are built outside frontend/ (bindings/wasm); allow Vite to serve them in
      // dev before they're copied/symlinked into public/ or imported as an asset.
      allow: ['..'],
    },
  },
  build: {
    target: 'es2022',
    rollupOptions: {
      // perf/index.html is the M17 follow-up performance-test harness (tests/e2e/perf.spec.ts
      // drives it via `vite preview`, which only serves what a build actually emitted) — not
      // part of the shipped app, but built alongside it so the Playwright perf spec can hit it.
      input: {
        main: fileURLToPath(new URL('./index.html', import.meta.url)),
        perf: fileURLToPath(new URL('./perf/index.html', import.meta.url)),
      },
    },
  },
  worker: {
    format: 'es',
  },
  test: {
    // tests/e2e/ holds Playwright specs (M17 follow-up perf harness) — a different `test()`
    // global entirely, and Vitest's default include glob would otherwise pick them up too.
    exclude: ['**/node_modules/**', 'tests/e2e/**'],
  },
});
