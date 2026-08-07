# Vendored third-party sources

Single-header/single-file libraries only. Vendored by committing the exact upstream file, not via
`FetchContent` — see M02 decision. Bump these deliberately; do not let them drift silently.

| Library | File | Upstream | Commit | Licence |
|---|---|---|---|---|
| dr_wav | `dr_wav.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| dr_flac | `dr_flac.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| dr_mp3 | `dr_mp3.h` | https://github.com/mackron/dr_libs | `50bb723e6a459dbb781e26cefee4fd9ca6714d6a` | public domain / MIT-0 (dual, see file header) |
| stb_vorbis | `stb_vorbis.c` | https://github.com/nothings/stb | `31c1ad37456438565541f4919958214b6e762fb4` | public domain / MIT (dual, see file header) |
| BLAKE3 | `blake3.h` | https://github.com/BLAKE3-team/BLAKE3 | TBD (use latest C single-file impl) | CC0 1.0 / Apache-2.0 (dual, see file header) |
| xxHash3 | `xxhash.h` | https://github.com/Cyan4973/xxHash | TBD (pinned to v0.8.1+ for C-only support) | BSD-2-Clause (see file header) |
| Catch2 | (FetchContent, not vendored) | https://github.com/catchorg/Catch2 | pinned tag in `tests/CMakeLists.txt` | BSL-1.0 |
| PocketFFT | `pocketfft_hdronly.h` | https://github.com/mreineck/pocketfft (`cpp` branch) | `c90e55b3d529f8efa40ed01a20de22405f45fc65` | BSD-3-Clause (full text embedded top-of-file, like the dr_libs/stb_vorbis headers above) |

## Update procedure

1. Pick the new upstream commit deliberately (not "latest" on a whim) — read the changelog.
2. Re-download the single file, overwrite in place, update the commit hash above.
3. Re-run the full test suite, including the ISO/format compliance fixtures (M23) before merging.
4. Vendored files are not modified in place; any local fix goes upstream or into a small adapter in
   `engine/decoder/` or `engine/cache/`, never as a patched copy that silently diverges from a re-downloadable original.

## Remaining vendor tasks (M16)

- [ ] Download `blake3.h` (C single-file implementation, not `blake3_c.h`) from BLAKE3 repo to `third_party/blake3.h`
- [ ] Download `xxhash.h` from xxHash repo to `third_party/xxhash.h`
- [ ] Update `engine/cache/hash.cpp` to include and call the vendored libraries (currently stubbed)
- [ ] Update this table with actual commit hashes once vendored

## Licence copies

Full licence text for each dual-licence header is embedded in the file itself (top-of-file comment
block for dr_libs; bottom-of-file for stb_vorbis). No separate `LICENSE` files are needed for
single-header public-domain/MIT-0 libraries; this table is the attribution record.
