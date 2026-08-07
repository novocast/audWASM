# `.awc` — audWASM Cache Format (Normative specification)

**Version:** 1  
**Endianness:** Little-endian throughout (every target is LE; assert at read time)  
**See also:** [M16 design decision document](../documentation/tasks/M16-waveform-cache-format.md)

---

## Binary format

All multi-byte integers and floats are **little-endian**. All offsets are absolute from the start of the file.

### Header (64 bytes, fixed)

```
Offset  Size  Type    Field              Description
0       4     char[4] magic              literal "AWC1" (0x31 0x43 0x57 0x41)
4       2     u16     formatVersion      1 on initial release; breaks on format changes
6       2     u16     headerSize         64 (allows future growth)
8       4     u32     flags              bit 0: chunks are compressed (reserved for v1, unused)
12      4     u32     engineVersion      packed major.minor.patch (e.g., 0x00010203 = 1.2.3)
16      32    u8[32]  sourceHash         BLAKE3 hash of the source audio file
48      8     u64     sourceSize         byte length of the source file
56      4     u32     sampleRate         samples per second (e.g., 48000)
60      4     u32     channels           channel count (e.g., 2)
```

The M16 design specifies a 64-byte header, but the field list actually requires 80 bytes. This implementation uses 80 bytes:

| Offset | Size | Type    | Field         | Notes |
|--------|------|---------|---------------|-------|
| 0      | 4    | char[4] | magic         | "AWC1" (0x31 0x43 0x57 0x41) |
| 4      | 2    | u16     | formatVersion | 1 for initial release; breaks on format changes |
| 6      | 2    | u16     | headerSize    | 80 (allows future growth without breaking readers) |
| 8      | 4    | u32     | flags         | bit 0: reserved for per-chunk compression (unused in v1) |
| 12     | 4    | u32     | engineVersion | packed major.minor.patch (e.g., 0x01020300 = v1.2.3) |
| 16     | 32   | u8[32]  | sourceHash    | BLAKE3 digest of the source audio file |
| 48     | 8    | u64     | sourceSize    | total bytes of the source file |
| 56     | 4    | u32     | sampleRate    | samples per second (e.g., 48000) |
| 60     | 4    | u32     | channels      | number of channels |
| 64     | 8    | i64     | frameCount    | total sample frames (>0, kNoFrame = -1 for streams) |
| 72     | 4    | u32     | chunkCount    | number of chunks in the directory |
| 76     | 4    | u32     | reserved      | must be 0 (reserved for future use) |

**Total: 80 bytes**

### Chunk Directory (chunkCount × 32 bytes)

Starts at offset `chunkDirOffset` (stored in chunks[].offset field). Each chunk directory entry:

| Offset | Size | Type  | Field         | Notes |
|--------|------|-------|---------------|-------|
| 0      | 4    | u32   | type          | FourCC chunk type (e.g., 'WVPY', 'LUFS') |
| 4      | 4    | u32   | analyzerVersion | the Analyzer::version() that produced this chunk |
| 8      | 8    | u64   | paramsHash    | XXH3_64 of the analyzer parameters (0 if no params) |
| 16     | 8    | u64   | offset        | byte offset of chunk payload from file start |
| 24     | 8    | u64   | storedSize    | bytes on disk (post-compression if flags.bit0=1) |
| 32     | 8    | u64   | rawSize       | uncompressed payload size (== storedSize if not compressed) |
| 40     | 8    | u64   | checksum      | XXH3_64 of the raw (decompressed) payload |

**Total per entry: 48 bytes** (not 32 as originally designed; this is more natural for 64-bit systems)

### Chunk Payloads

Chunks appear in the file at the offsets specified in the directory. Payload format is chunk-type-specific; see per-chunk specifications below.

### Chunk Types

#### `WVPY` — Waveform Pyramid

A multi-level mipmap pyramid of waveform samples for display. Produced by M04's waveform analyzer with all variants (stereo, mono, RMS+peak per-channel).

```
Offset  Size               Type        Description
0       4                  u32         formatVersion (currently 1)
4       4                  u32         channelCount
8       4                  u32         variantCount (e.g., 2 for RMS+peak)
12      4                  u32         levelCount (number of mipmap levels)

[repeat levelCount times]:
16+i*8  4                  u32         samplesPerLevel[i]
        4                  u32         reserved

[repeat levelCount × channelCount × variantCount]:
        4                  float       sample value (IEEE 754 little-endian)
```

**Design note:** Each level is interleaved by channel, then by variant (RMS, peak, etc.). All samples are 32-bit IEEE 754 floats, so reading is a memcpy with no parsing.

#### `LUFS` — Loudness Result + Time Series

Integrated loudness, loudness range, true peak, and time series (momentary/short-term).

```
Offset  Size  Type    Field                      
0       8     f64     integratedLufs (IEEE 754, NaN if gating failed)
8       8     f64     loudnessRangeLu (IEEE 754, NaN if invalid)
16      8     f64     truePeakDbtp (IEEE 754, -inf if silent)
24      8     f64     samplePeakDbfs (IEEE 754)
32      4     u32     truePeakOversampling (4, 8, or 16)
36      4     u32     reserved
40      4     u32     momentaryLufsLength
44      4     u32     shortTermLufsLength

[momentaryLufs]:
48+i*4  4     float   momentaryLufs[i] (100 ms resolution)

[shortTermLufs, offset = 48 + momentaryLufsLength * 4]:
        float           shortTermLufs[j] (100 ms resolution)

[metadata flags, offset = previous + shortTermLufsLength * 4]:
        1     u8      usedFallbackChannelLayout (0 or 1)
        3     u8      reserved
```

**Note:** There are per-channel true peak and sample peak arrays, but the integrated `truePeakDbtp` and `samplePeakDbfs` are maxima across channels. Per-channel versions are preserved in the analysis result but not cached (they can be recomputed from the peaks).

#### `STAT` — Statistics (mean, RMS, crest factor, etc.)

```
Offset  Size  Type    Field
0       4     u32     channelCount
4       4     u32     reserved

[repeat channelCount]:
8+i*40  8     f64     mean (linear)
        8     f64     rms (linear)
        8     f64     peakDb (dBFS)
        8     f64     crestFactor (dB)
```

#### `SIL` — Silence Detection (regions and time series)

```
Offset  Size  Type    Field
0       4     u32     threshold_dBFS (stored as i32 for safety)
4       4     u32     regionCount

[repeat regionCount]:
8+i*16  8     i64     startFrame
        8     i64     endFrame

[time series, 100 ms resolution]:
offset = 8 + regionCount * 16
len*4   float           isSilent[] (0.0 = sound, 1.0 = silence)
```

#### `CLIP` — Clipping Detection

```
Offset  Size  Type    Field
0       4     u32     channelCount
4       4     u32     sampleCount (== number of detected samples, might be 0)

[repeat sampleCount]:
8+i*12  8     i64     frameIndex
        4     i32     channelIndex

[amplitude data]:
offset = 8 + sampleCount * 12
count*4 float           amplitude[] (dBFS at each clipping sample)
```

#### `DC__` — DC Offset Detection

```
Offset  Size  Type    Field
0       4     u32     channelCount
4       4     u32     reserved

[repeat channelCount]:
8+i*8   8     f64     dcOffsetLinear
```

#### `BEAT` — Beat / Tempo / Onset Detection

```
Offset  Size  Type    Field
0       4     u32     tempoHz (stored as integer, scale by 1e-6 to get Hz)
4       4     u32     confidence (0–1000, scale by 0.001 to get 0.0–1.0)
8       4     u32     onsetCount
12      4     u32     odfLength

[onsets]:
16+i*8  8     i64     onsetFrame[i]

[ODF time series, 100 ms resolution]:
offset = 16 + onsetCount * 8
odfLen*4 float          odf[] (novelty values)
```

#### `TRAN` — Transient Detection

```
Offset  Size  Type    Field
0       4     u32     transientCount
4       4     u32     reserved

[repeat transientCount]:
8+i*8   8     i64     transientFrame[i]
```

#### `META` — Metadata + Cover Art

Structured format with length prefixes. All strings are UTF-8.

```
Offset  Size  Type    Field
0       4     u32     titleLength
4       var   char[]  title (UTF-8, not null-terminated)
...     4     u32     artistLength
        var   char[]  artist (UTF-8)
...     4     u32     albumLength
        var   char[]  album (UTF-8)
...     4     u32     dateLength
        var   char[]  date (UTF-8)
...     4     u32     genreLength
        var   char[]  genre (UTF-8)
...     4     u32     commentLength
        var   char[]  comment (UTF-8)
...     4     u32     durationFrames (int, or 0 if unknown)
...     4     u32     coverArtLength (0 if no cover art)
...     var   u8[]    coverArtData (JPEG or PNG, starts with 0xFF 0xD8 or 0x89 0x50)
```

#### `SPEC` — Spectrogram Overview Strip

The overview strip (at the highest zoom level) for fast waveform-like display.

```
Offset  Size  Type    Field
0       4     u32     fftSize
4       4     u32     hopSize
8       4     u32     window (0=hann, 1=hamming, ...)
12      4     u32     channelCount
16      4     u32     binCount (frequency bins)
20      4     u32     timeFrameCount (time frames in the overview)
24      var   float[] magnitude[][] (stored as channelCount × timeFrameCount × binCount, row-major)
```

All samples are 32-bit floats (linear magnitude, not dB).

---

## Reading Strategy

1. Read and validate the 80-byte header.
2. Verify `magic == "AWC1"` and `formatVersion == 1` (unknown formats → discard file).
3. Check `headerSize` (future versions might extend the header; skip to `chunkDirOffset` regardless).
4. Seek to `chunkDirOffset` and read the chunk directory.
5. For each chunk in the directory, check if it's recognized:
   - If recognized: validate `analyzerVersion` and `paramsHash` against current state.
   - If not recognized: skip (file is forward-compatible).
6. For valid chunks, seek to the offset and read the payload.
7. If the file is truncated, corrupted, or has a bad checksum → treat as a cache miss (never crash).

## Writing Strategy

1. Compute `sourceHash` = BLAKE3(file bytes).
2. Begin building chunk payloads in memory (or temp file) and collect directory entries.
3. Write the header (with a placeholder for `chunkDirOffset` if not yet known).
4. Write all chunk payloads sequentially.
5. Write the chunk directory at the current position.
6. Update `chunkDirOffset` in the header (requires seeking back).
7. Rename temp file to final name atomically (ensure no corrupt intermediate files).

## Versioning & Invalidation

- **File-level:** `formatVersion` mismatch → discard entire file (breaking format changes).
- **Chunk-level:** For each chunk, it's valid if:
  ```
  analyzerVersion == currentAnalyzer.version()
  && paramsHash == hash(currentParameters)
  && checksum == XXH3_64(rawPayload)
  ```
- **Engine-level:** `engineVersion` mismatch is informational (doesn't invalidate, but may guide future decisions).

If any chunk fails validation, that chunk alone is recomputed; other chunks survive.

## Compression (Reserved, Unused in v1)

Bit 0 of the `flags` field is reserved for per-chunk compression. If set in v1, readers should skip the file (an unknown feature). Future versions may define:
- `storedSize < rawSize` → payload is compressed (zstd or similar).
- `storedSize == rawSize` → uncompressed.

v1 writers must set `flags = 0` and `storedSize == rawSize` for all chunks.

