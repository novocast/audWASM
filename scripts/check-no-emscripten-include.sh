#!/usr/bin/env bash
# Fails if any file under engine/ includes <emscripten.h> (or any emscripten/*.h) — see M00 §4.
# Platform escape hatches must go through engine/util/platform.hpp instead.
set -euo pipefail

cd "$(dirname "$0")/.."

if grep -rEn '#include\s*[<"]emscripten' engine/; then
    echo ""
    echo "error: engine/ must never include Emscripten headers directly (see M00 §4)." >&2
    echo "Add a platform escape hatch to engine/util/platform.hpp instead." >&2
    exit 1
fi

echo "OK: no Emscripten includes under engine/"
