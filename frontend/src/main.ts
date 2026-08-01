import { AudioEngine } from '../../bindings/wasm/engine.ts';

async function main(): Promise<void> {
  const statusEl = document.querySelector<HTMLParagraphElement>('#status')!;
  const infoEl = document.querySelector<HTMLDListElement>('#build-info')!;

  try {
    const engine = await AudioEngine.create();
    const pass = engine.runSelfTest();

    statusEl.textContent = pass ? 'Engine self-test: PASS' : 'Engine self-test: FAIL';
    statusEl.style.color = pass ? 'seagreen' : 'crimson';

    const info = engine.buildInfo;
    infoEl.innerHTML = '';
    for (const [key, value] of Object.entries({ version: engine.version, ...info })) {
      const dt = document.createElement('dt');
      dt.textContent = key;
      const dd = document.createElement('dd');
      dd.textContent = String(value);
      infoEl.append(dt, dd);
    }
  } catch (err) {
    statusEl.textContent = `Engine failed to load: ${(err as Error).message}`;
    statusEl.style.color = 'crimson';
  }
}

void main();
