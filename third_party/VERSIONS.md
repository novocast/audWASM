# Vendored third-party sources

Single-header/single-file libraries only. Vendored by committing the exact upstream file, not via
`FetchContent` — see M02 decision. Bump these deliberately; do not let them drift silently.

| Library | File | Upstream | Commit | Licence |
|---|---|---|---|---|
| dr_wav | `dr_wav.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| dr_flac | `dr_flac.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| dr_mp3 | `dr_mp3.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| stb_vorbis | `stb_vorbis.c` | https://github.com/nothings/stb | `31c1ad37456438565541f4919958214b6e762fb4` | public domain / MIT (dual, see file header) |
| Catch2 | (FetchContent, not vendored) | https://github.com/catchorg/Catch2 | pinned tag in `tests/CMakeLists.txt` | BSL-1.0 |

## Update procedure

1. Pick the new upstream commit deliberately (not "latest" on a whim) — read the changelog.
2. Re-download the single file, overwrite in place, update the commit hash above.
3. Re-run the full test suite, including the ISO/format compliance fixtures (M23) before merging.
4. Vendored files are not modified in place; any local fix goes upstream or into a small adapter in
   `engine/decoder/`, never as a patched copy that silently diverges from a re-downloadable original.

## Licence copies

Full licence text for each dual-licence header is embedded in the file itself (top-of-file comment
block for dr_libs; bottom-of-file for stb_vorbis). No separate `LICENSE` files are needed for
single-header public-domain/MIT-0 libraries; this table is the attribution record.
