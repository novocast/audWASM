// Minimal reference driver against libebur128 (https://github.com/jiixyj/libebur128), used only for
// M08's cross-validation task ("Cross-validation against ffmpeg -af ebur128 and libebur128 on the
// fixture corpus") — NOT part of the shipped engine or build. Reads raw interleaved float32 PCM
// (produced by ffmpeg from whatever the source container is) plus its sample rate/channel count on
// the command line, feeds it through libebur128, and prints integrated LUFS / LRA / true peak
// (max over channels, dBTP) / sample peak (max over channels, dBFS) as CSV.
//
// Usage: ebur128_ref <raw_f32_path> <sampleRate> <channels>
//
// Build (from tests/fixtures/ebu_loudness_test_set, or anywhere ffmpeg output lands):
//   gcc -O2 -I ~/libebur128/ebur128 tools/compliance/ebur128_ref.c \
//       -L ~/libebur128/build -lebur128 -lm -o /tmp/ebur128_ref \
//       -Wl,-rpath,$HOME/libebur128/build

#include <ebur128.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static double linear_to_db(double linear) {
    return linear > 0.0 ? 20.0 * log10(linear) : -INFINITY;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <raw_f32_path> <sampleRate> <channels>\n", argv[0]);
        return 2;
    }
    const char*  path       = argv[1];
    unsigned int sampleRate = (unsigned int)atoi(argv[2]);
    unsigned int channels   = (unsigned int)atoi(argv[3]);

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    ebur128_state* st = ebur128_init(
        channels, sampleRate,
        EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_SAMPLE_PEAK);
    if (!st) {
        fprintf(stderr, "ebur128_init failed\n");
        return 1;
    }

    // Feed in 100 ms chunks and poll momentary/short-term after each — the same hop our own
    // engine uses for its momentary/short-term series (see M08's design doc) — so "Max M"/"Max S"
    // over a file is comparable apples-to-apples, including the same hop-quantization artefacts,
    // rather than comparing our hop-quantized max against a hypothetical continuously-sliding one.
    const size_t chunkFrames = (size_t)sampleRate / 10;
    float* buf = (float*)malloc(chunkFrames * channels * sizeof(float));
    double maxMomentary = -INFINITY, maxShortTerm = -INFINITY;

    size_t framesRead;
    while ((framesRead = fread(buf, sizeof(float) * channels, chunkFrames, f)) > 0) {
        if (ebur128_add_frames_float(st, buf, framesRead) != EBUR128_SUCCESS) {
            fprintf(stderr, "ebur128_add_frames_float failed\n");
            return 1;
        }
        double m = -INFINITY, s = -INFINITY;
        ebur128_loudness_momentary(st, &m);
        ebur128_loudness_shortterm(st, &s);
        if (m > maxMomentary) maxMomentary = m;
        if (s > maxShortTerm) maxShortTerm = s;
    }
    free(buf);
    fclose(f);

    double integratedLufs = NAN, lra = NAN;
    ebur128_loudness_global(st, &integratedLufs);
    ebur128_loudness_range(st, &lra);

    double truePeakMax = 0.0, samplePeakMax = 0.0;
    for (unsigned int ch = 0; ch < channels; ++ch) {
        double tp = 0.0, sp = 0.0;
        ebur128_true_peak(st, ch, &tp);
        ebur128_sample_peak(st, ch, &sp);
        if (tp > truePeakMax) truePeakMax = tp;
        if (sp > samplePeakMax) samplePeakMax = sp;
    }

    printf("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", integratedLufs, lra, linear_to_db(truePeakMax),
           linear_to_db(samplePeakMax), maxMomentary, maxShortTerm);

    ebur128_destroy(&st);
    return 0;
}
