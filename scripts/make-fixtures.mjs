#!/usr/bin/env node
// Generates the synthetic test fixtures described in M02/M23: tones, sweeps, silence, and clipped
// material, at several sample rates and bit depths, encoded to every supported format.
//
// Requires ffmpeg locally (not in CI — the generated files are committed to tests/fixtures/ so CI
// needs no ffmpeg, per M02's decision that native and Node test suites read the same fixture data
// off disk).
//
// Usage: node scripts/make-fixtures.mjs [--out tests/fixtures]

import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';

function parseArgs(argv) {
  const args = { out: 'tests/fixtures' };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i] === '--out') args.out = argv[++i];
  }
  return args;
}

const SAMPLE_RATES = [44100, 48000, 96000];
const BIT_DEPTHS = { wav: [16, 24, 32], flac: [16, 24] };
const FORMATS = ['wav', 'flac', 'mp3', 'ogg'];

// [name, ffmpeg -f lavfi filter]
const SOURCES = [
  ['tone_1khz', 'sine=frequency=1000:duration=2'],
  ['sweep_20_20k', 'aevalsrc=0.5*sin(2*PI*(20+((20000-20)/10)*t)*t):duration=10'],
  ['silence', 'anullsrc=r={rate}:cl=stereo:duration=2'],
  // Clipped: sine amplified well past full scale, relies on ffmpeg NOT soft-limiting so the encoded
  // file genuinely contains inter-sample/digital clipping for M11 to detect.
  ['clipped_1khz', 'sine=frequency=1000:duration=2,volume=6'],
];

function ffmpeg(args) {
  execFileSync('ffmpeg', ['-y', '-hide_banner', '-loglevel', 'error', ...args], { stdio: 'inherit' });
}

function main() {
  const { out } = parseArgs(process.argv.slice(2));
  mkdirSync(out, { recursive: true });

  for (const rate of SAMPLE_RATES) {
    for (const [name, filterTemplate] of SOURCES) {
      const filter = filterTemplate.replace('{rate}', String(rate));
      for (const format of FORMATS) {
        const depths = BIT_DEPTHS[format] ?? [null];
        for (const depth of depths) {
          const depthSuffix = depth ? `_${depth}bit` : '';
          const filename = `${name}_${rate}hz${depthSuffix}.${format}`;
          const codecArgs = [];
          if (format === 'wav' && depth) {
            codecArgs.push('-c:a', depth === 32 ? 'pcm_f32le' : `pcm_s${depth}le`);
          } else if (format === 'flac' && depth) {
            codecArgs.push('-sample_fmt', depth === 24 ? 's32' : 's16', '-bits_per_raw_sample', String(depth));
          } else if (format === 'mp3') {
            codecArgs.push('-c:a', 'libmp3lame', '-b:a', '320k');
          } else if (format === 'ogg') {
            codecArgs.push('-c:a', 'libvorbis', '-q:a', '6');
          }

          console.log(`generating ${filename}`);
          ffmpeg(['-f', 'lavfi', '-i', filter, '-ar', String(rate), '-ac', '2', ...codecArgs, join(out, filename)]);
        }
      }
    }
  }

  console.log(`\nDone. Commit the contents of ${out}/ (see .gitattributes for -diff/-text handling).`);
}

main();
