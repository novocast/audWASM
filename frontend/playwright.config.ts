import { defineConfig } from '@playwright/test';

// M17 follow-up ("automated performance test"): drives tests/e2e/perf.spec.ts against
// perf/index.html (see that file's header comment for what it does and doesn't cover). Playwright
// itself was already a devDependency (per the M17 doc's original scope note) but had no config or
// spec wired up until this pass.
export default defineConfig({
  testDir: 'tests/e2e',
  timeout: 30_000,
  fullyParallel: false,
  reporter: [['list']],
  use: {
    baseURL: 'http://localhost:4173',
  },
  webServer: {
    // `preview`, not `dev`: a production-ish build is what the 60fps acceptance criterion is
    // actually about — the dev server's unminified/unbundled modules would understate perf.
    command: 'npm run build && npm run preview -- --port 4173 --strictPort',
    url: 'http://localhost:4173',
    reuseExistingServer: !process.env.CI,
    timeout: 120_000,
  },
});
