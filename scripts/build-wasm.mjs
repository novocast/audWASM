#!/usr/bin/env node
// Single-step WASM build + deploy: configures and builds the aud_wasm CMake target, then copies
// the resulting aud_wasm.{js,wasm} into bindings/wasm/ — the location engine.ts actually imports
// from (`import createAudModule from './aud_wasm.js'`). CMake only ever writes its build output
// under build/<preset>/, and nothing else in the repo copies it into bindings/wasm/, which is the
// exact gap that produces a confusing "module.Transport is undefined" (or similarly missing-symbol)
// error in the browser after a source change: the frontend keeps loading a stale copy until this
// copy step runs.
//
// Usage:
//   node scripts/build-wasm.mjs [--debug | --profile | --release] [--preset <name>]
//
// Defaults to the wasm-release preset. See README.md "Building the WASM module".

import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..');

function parsePreset(argv) {
  const explicit = argv.indexOf('--preset');
  if (explicit !== -1 && argv[explicit + 1]) {
    return argv[explicit + 1];
  }
  if (argv.includes('--debug')) return 'wasm-debug';
  if (argv.includes('--profile')) return 'wasm-profile';
  return 'wasm-release'; // --release or no flag
}

function run(command, args) {
  console.log(`\n$ ${command} ${args.join(' ')}`);
  const result = spawnSync(command, args, { cwd: repoRoot, stdio: 'inherit', shell: process.platform === 'win32' });
  if (result.error) {
    throw result.error;
  }
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function main() {
  const preset = parsePreset(process.argv.slice(2));

  if (!process.env.EMSDK) {
    console.warn(
      'warning: $EMSDK is not set. The wasm-* presets need it (source emsdk_env.sh first) — ' +
        'proceeding anyway in case it was already configured with a prior EMSDK value baked into the cache.',
    );
  }

  run('cmake', ['--preset', preset]);
  run('cmake', ['--build', '--preset', preset]);

  const builtDir = join(repoRoot, 'build', preset, 'bindings', 'wasm');
  const destDir = join(repoRoot, 'bindings', 'wasm');
  const files = ['aud_wasm.js', 'aud_wasm.wasm'];

  for (const file of files) {
    const src = join(builtDir, file);
    if (!existsSync(src)) {
      console.error(`error: expected build output missing: ${src}`);
      process.exit(1);
    }
    copyFileSync(src, join(destDir, file));
    const { size } = statSync(src);
    console.log(`copied ${file} (${(size / 1024).toFixed(1)} KB) -> bindings/wasm/${file}`);
  }

  console.log(
    '\nDone. Hard-reload the frontend dev server tab (Vite may have cached the old .wasm) before testing.',
  );
}

main();
