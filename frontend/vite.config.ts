import { defineConfig } from 'vite';

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
  },
  worker: {
    format: 'es',
  },
});
