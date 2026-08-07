// Live DPR-change response (M17 "Device pixel ratio": "Handle DPR changes (dragging between
// monitors) via matchMedia('(resolution: ...)') — polling devicePixelRatio misses it").
//
// matchMedia's own change event only fires once per listener (the spec re-evaluates the query
// text against the *new* resolution, which usually makes it stop matching) — so each firing tears
// down and re-registers a fresh listener at the current devicePixelRatio.

export function watchDevicePixelRatio(onChange: (dpr: number) => void): () => void {
  let unsubscribe: (() => void) | null = null;

  const register = (): void => {
    const dpr = window.devicePixelRatio;
    const query = matchMedia(`(resolution: ${dpr}dppx)`);
    const handleChange = (): void => {
      onChange(window.devicePixelRatio);
      register(); // re-arm at the new DPR
    };
    query.addEventListener('change', handleChange);
    unsubscribe = () => query.removeEventListener('change', handleChange);
  };

  register();
  return () => unsubscribe?.();
}
