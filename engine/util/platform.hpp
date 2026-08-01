#pragma once

// The one escape hatch for platform-specific behaviour. See M00 §4:
// "the engine/ tree must not #include <emscripten.h> anywhere." Everything that differs between
// native and WASM targets is declared here and implemented per-target under engine/util/platform/;
// CI's `lint` job greps the whole engine/ tree for the forbidden include (see scripts/check-no-emscripten-include).
//
// In practice, almost nothing needs a real per-platform implementation: stderr writes are
// redirected to the browser console by the Emscripten runtime without any explicit include, and
// std::abort() traps cleanly under -fno-exceptions. Prefer adding to this file only when a genuine
// platform divergence appears (e.g. a high-resolution clock source, or SharedArrayBuffer detection
// post-M22).

namespace aud::platform {

// Triggers a debugger breakpoint if one is attached; a no-op trap otherwise. Never used for
// input-driven error paths (those are aud::Error) — debug/programmer-error tooling only.
void debugBreak() noexcept;

}  // namespace aud::platform
