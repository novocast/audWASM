#!/usr/bin/env node
// Reports gzip/brotli sizes of the built .wasm as a CI job-summary table (M01 `size` job).
// Usage: node scripts/size-report.mjs <path-to.wasm> [--fail-over-percent N --baseline-bytes N]

import { readFileSync } from 'node:fs';
import { gzipSync, brotliCompressSync } from 'node:zlib';

function parseArgs(argv) {
  const args = { failOverPercent: 5, baselineBytes: null, wasmPath: null };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--fail-over-percent') {
      args.failOverPercent = Number(argv[++i]);
    } else if (arg === '--baseline-bytes') {
      args.baselineBytes = Number(argv[++i]);
    } else if (!args.wasmPath) {
      args.wasmPath = arg;
    }
  }
  return args;
}

function formatBytes(bytes) {
  return `${(bytes / 1024).toFixed(1)} KB`;
}

async function main() {
  const { wasmPath, failOverPercent, baselineBytes } = parseArgs(process.argv.slice(2));
  if (!wasmPath) {
    console.error('usage: size-report.mjs <path-to.wasm> [--fail-over-percent N] [--baseline-bytes N]');
    process.exit(2);
  }

  const raw = readFileSync(wasmPath);
  const gzip = gzipSync(raw, { level: 9 });
  const brotli = brotliCompressSync(raw);

  const rows = [
    ['raw', raw.length],
    ['gzip', gzip.length],
    ['brotli', brotli.length],
  ];

  console.log('| Encoding | Size |');
  console.log('|---|---|');
  for (const [name, size] of rows) {
    console.log(`| ${name} | ${formatBytes(size)} |`);
  }

  // Write to the GitHub Actions job summary if available.
  if (process.env.GITHUB_STEP_SUMMARY) {
    const { appendFileSync } = await import('node:fs');
    let summary = `### Wasm size report (${wasmPath})\n\n| Encoding | Size |\n|---|---|\n`;
    for (const [name, size] of rows) {
      summary += `| ${name} | ${formatBytes(size)} |\n`;
    }
    appendFileSync(process.env.GITHUB_STEP_SUMMARY, summary);
  }

  if (baselineBytes !== null) {
    const growthPercent = ((gzip.length - baselineBytes) / baselineBytes) * 100;
    console.log(`\ngzip growth vs baseline: ${growthPercent.toFixed(2)}%`);
    if (growthPercent > failOverPercent) {
      console.error(
        `error: gzip size grew ${growthPercent.toFixed(2)}% vs baseline, exceeding the ${failOverPercent}% gate (M01 'size' job). Add a 'size-ok' label to override.`,
      );
      process.exit(1);
    }
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
