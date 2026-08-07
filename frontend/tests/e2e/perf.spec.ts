import { test, expect } from '@playwright/test';

// M17 acceptance criterion: "60 fps sustained during playback with waveform + all overlays,
// measured over a 60 s run with a p99 frame time under 16.6 ms." Previously verified manually only
// (see the M17 doc's "Known follow-ups"). This automates it against perf/index.html's synthetic
// 1-hour-view harness (see that file's header comment for exactly what it does and doesn't cover:
// the render loop under a real workload shape, not decode).
//
// The measurement window here is shorter than the milestone's "60s run" wording — long enough
// (FrameTimeStats's 300-frame ring buffer fills several times over within it) to be a real
// steady-state measurement rather than a cold-start sample, while keeping CI time bounded. A
// longer soak is a `kRunMs` bump away in perf.ts if that tradeoff ever needs to move.
test('render loop sustains p99 < 16.6ms with waveform + all overlays on a synthetic 1-hour view', async ({ page }) => {
  await page.goto('/perf/index.html');

  await page.waitForFunction(() => document.querySelector('#perf-result')?.getAttribute('data-done') === 'true', {
    timeout: 20_000,
  });

  const resultText = await page.locator('#perf-result').textContent();
  expect(resultText).toBeTruthy();
  const stats = JSON.parse(resultText!) as { count: number; averageMs: number; p99Ms: number; maxMs: number };

  expect(stats.count).toBeGreaterThan(60); // sanity: the loop actually ran, not a single stale frame
  expect(stats.p99Ms).toBeLessThan(16.6);
});
