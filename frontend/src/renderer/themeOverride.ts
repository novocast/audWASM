// Manual theme override for visually confirming light/dark/high-contrast end-to-end (M17 follow-up
// "theme visual verification" — the demo previously had no way to see a theme other than whatever
// the OS happened to be set to). Mirrors backendSelection.ts's override pattern exactly: a
// `?audTheme=` query param (checked first, so a shared link reproduces what was seen) or a sticky
// `localStorage` toggle (checked second), falling back to 'auto' (i.e. respect the OS/media-query
// theme, theme.css's default behaviour).

export type ThemeOverride = 'auto' | 'light' | 'dark' | 'highContrast';

const kOverrideQueryParam = 'audTheme';
const kOverrideStorageKey = 'aud.theme.override';
const kValidOverrides: readonly ThemeOverride[] = ['light', 'dark', 'highContrast'];

function isThemeOverride(value: string | null): value is Exclude<ThemeOverride, 'auto'> {
  return (kValidOverrides as readonly string[]).includes(value ?? '');
}

export function readThemeOverride(location: Location = window.location): ThemeOverride {
  const param = new URLSearchParams(location.search).get(kOverrideQueryParam);
  if (isThemeOverride(param)) return param;
  try {
    const stored = localStorage.getItem(kOverrideStorageKey);
    if (isThemeOverride(stored)) return stored;
  } catch {
    // localStorage can throw in restricted contexts (private browsing quotas, etc.) — 'auto' is a
    // perfectly good fallback, not a failure worth surfacing.
  }
  return 'auto';
}

export function setThemeOverride(override: ThemeOverride): void {
  try {
    if (override === 'auto') localStorage.removeItem(kOverrideStorageKey);
    else localStorage.setItem(kOverrideStorageKey, override);
  } catch {
    // Same rationale as above — best-effort persistence, never a hard requirement.
  }
}

/** Applies (or clears) the override as `data-theme` on `<html>` — theme.css's `:root[data-theme]`
 *  rules take it from there (attribute selectors outrank the plain media-query rules, so this
 *  always wins over the OS setting when set). */
export function applyThemeOverride(override: ThemeOverride, root: HTMLElement = document.documentElement): void {
  if (override === 'auto') root.removeAttribute('data-theme');
  else root.dataset.theme = override;
}
