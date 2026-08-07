#!/usr/bin/env python3
"""M08 compliance runner: EBU Tech 3341 / Tech 3342 test cases, cross-validated against
ffmpeg's `-af ebur128` filter and libebur128, run against our own LoudnessAnalyzer via the
`loudness_ref` driver (tools/compliance/loudness_ref.cpp).

Not part of the shipped engine, CI, or build — a one-off / repeatable verification script for this
ticket's acceptance criteria. Expects:
  - FIXTURES_DIR to contain the EBU loudness test set (tests/fixtures/ebu_loudness_test_set/,
    downloaded manually per the ticket's notes; not committed to git).
  - LOUDNESS_REF, EBUR128_REF pointing at the two compiled driver binaries.
  - ffmpeg/ffprobe on PATH.

Test cases and expected values transcribed from EBU Tech 3341 (2023) Table 1 and EBU Tech 3342
(2023) Table 1 — see tools/compliance/README.md for how those were obtained and double-checked.
"""
import csv
import io
import os
import re
import subprocess
import sys
import tempfile

FIXTURES_DIR = os.environ.get(
    "FIXTURES_DIR", "/mnt/d/Projects/audWASM/tests/fixtures/ebu_loudness_test_set"
)
LOUDNESS_REF = os.environ.get("LOUDNESS_REF", "/tmp/loudness_ref")
EBUR128_REF = os.environ.get("EBUR128_REF", "/tmp/ebur128_ref")


def path(name):
    return os.path.join(FIXTURES_DIR, name)


# ---------------------------------------------------------------------------------------------
# EBU Tech 3341 Table 1 (test cases 1-23) + Tech 3342 Table 1 (test cases 1-6), mapped to the
# actual filenames in the v05 test set. metric is one of: "I" (integrated LUFS), "LRA" (LU),
# "TP" (true peak dBTP, tolerance is a (low, high) offset pair, not symmetric).
# ---------------------------------------------------------------------------------------------
CASES = [
    # --- Tech 3341 Table 1: Integrated loudness ---
    dict(case="3341-1", file="seq-3341-1-16bit.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-2", file="seq-3341-2-16bit.wav", metric="I", expected=-33.0, tol=0.1),
    dict(case="3341-3", file="seq-3341-3-16bit-v02.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-4", file="seq-3341-4-16bit-v02.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-5", file="seq-3341-5-16bit-v02.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-6 (5.0)", file="seq-3341-6-5channels-16bit.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-6 (5.1 WAVEEX)", file="seq-3341-6-6channels-WAVEEX-16bit.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-7 (NLR programme)", file="seq-3341-7_seq-3342-5-24bit.wav", metric="I", expected=-23.0, tol=0.1),
    dict(case="3341-8 (WLR programme)", file="seq-3341-2011-8_seq-3342-6-24bit-v02.wav", metric="I", expected=-23.0, tol=0.1),

    # --- Tech 3341 Table 1: true peak (tolerance is +hi/-lo dB around `expected`) ---
    dict(case="3341-15", file="seq-3341-15-24bit.wav.wav", metric="TP", expected=-6.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-16", file="seq-3341-16-24bit.wav.wav", metric="TP", expected=-6.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-17", file="seq-3341-17-24bit.wav.wav", metric="TP", expected=-6.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-18", file="seq-3341-18-24bit.wav.wav", metric="TP", expected=-6.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-19", file="seq-3341-19-24bit.wav.wav", metric="TP", expected=3.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-20", file="seq-3341-20-24bit.wav.wav", metric="TP", expected=0.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-21", file="seq-3341-21-24bit.wav.wav", metric="TP", expected=0.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-22", file="seq-3341-22-24bit.wav.wav", metric="TP", expected=0.0, tol_lo=0.4, tol_hi=0.2),
    dict(case="3341-23", file="seq-3341-23-24bit.wav.wav", metric="TP", expected=0.0, tol_lo=0.4, tol_hi=0.2),

    # --- Tech 3342 Table 1: Loudness Range ---
    dict(case="3342-1", file="seq-3342-1-16bit.wav", metric="LRA", expected=10.0, tol=1.0),
    dict(case="3342-2", file="seq-3342-2-16bit.wav", metric="LRA", expected=5.0, tol=1.0),
    dict(case="3342-3", file="seq-3342-3-16bit.wav", metric="LRA", expected=20.0, tol=1.0),
    dict(case="3342-4", file="seq-3342-4-16bit.wav", metric="LRA", expected=15.0, tol=1.0),
    dict(case="3342-5 (NLR, shared w/ 3341-7)", file="seq-3341-7_seq-3342-5-24bit.wav", metric="LRA", expected=5.0, tol=1.0),
    dict(case="3342-6 (WLR, shared w/ 3341-8)", file="seq-3341-2011-8_seq-3342-6-24bit-v02.wav", metric="LRA", expected=15.0, tol=1.0),

    # --- Bonus calibration files (filename states the target, not from Table 1 directly) ---
    dict(case="calib -20 LUFS", file="1kHz Sine -20 LUFS-16bit.wav", metric="I", expected=-20.0, tol=0.1),
    dict(case="calib -26 LUFS", file="1kHz Sine -26 LUFS-16bit.wav", metric="I", expected=-26.0, tol=0.1),
    dict(case="calib -40 LUFS", file="1kHz Sine -40 LUFS-16bit.wav", metric="I", expected=-40.0, tol=0.1),
]

# --- File-based multi-file cases (3341-10, 3341-13): "Max S"/"Max M" per segment file ---
for i in range(1, 21):
    CASES.append(dict(case=f"3341-10 seg{i} (max S)", file=f"seq-3341-10-{i}-24bit.wav", metric="maxS",
                       expected=-23.0, tol=0.1))
for i in range(1, 21):
    # The vendored fixture set's own naming is inconsistent here: segments 1-2 are named
    # "...-24bit.wav", segments 3-20 are (accidentally, on the EBU's side) "...-24bit.wav.wav".
    suffix = "-24bit.wav" if i in (1, 2) else "-24bit.wav.wav"
    CASES.append(dict(case=f"3341-13 seg{i} (max M)", file=f"seq-3341-13-{i}{suffix}", metric="maxM",
                       expected=-23.0, tol=0.1))


def ffprobe_rate_channels(wav_path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
         "stream=sample_rate,channels", "-of", "csv=p=0", wav_path],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    rate, channels = out.split(",")
    return int(rate), int(channels)


def run_loudness_ref(wav_path):
    out = subprocess.run([LOUDNESS_REF, wav_path], capture_output=True, text=True)
    if out.returncode != 0:
        return None, out.stderr.strip()
    lines = out.stdout.strip("\n").split("\n")
    integrated, lra, tp, sp, oversampling, fallback = next(csv.reader(io.StringIO(lines[0])))
    momentary = [float(x) for x in lines[1].split(",")] if len(lines) > 1 and lines[1] else []
    shortterm = [float(x) for x in lines[2].split(",")] if len(lines) > 2 and lines[2] else []
    return dict(I=float(integrated), LRA=float(lra), TP=float(tp), SP=float(sp),
                maxM=max(momentary) if momentary else float("nan"),
                maxS=max(shortterm) if shortterm else float("nan")), None


def run_ebur128_ref(wav_path):
    rate, channels = ffprobe_rate_channels(wav_path)
    with tempfile.NamedTemporaryFile(suffix=".f32") as raw:
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y", "-i", wav_path, "-f", "f32le", "-acodec", "pcm_f32le", raw.name],
            check=True,
        )
        out = subprocess.run([EBUR128_REF, raw.name, str(rate), str(channels)],
                              capture_output=True, text=True, check=True).stdout.strip()
    integrated, lra, tp, sp, maxm, maxs = next(csv.reader(io.StringIO(out)))
    return dict(I=float(integrated), LRA=float(lra), TP=float(tp), SP=float(sp),
                maxM=float(maxm), maxS=float(maxs))


def run_ffmpeg_summary(wav_path):
    proc = subprocess.run(
        ["ffmpeg", "-v", "info", "-i", wav_path, "-af", "ebur128=peak=true", "-f", "null", "-"],
        capture_output=True, text=True,
    )
    text = proc.stderr
    m_i = re.search(r"I:\s*(-?[\d.]+)\s*LUFS", text.split("Summary:")[-1])
    m_lra = re.search(r"LRA:\s*(-?[\d.]+)\s*LU", text.split("Summary:")[-1])
    m_tp = re.search(r"Peak:\s*(-?[\d.]+)\s*dBFS", text.split("Summary:")[-1])
    return dict(
        I=float(m_i.group(1)) if m_i else None,
        LRA=float(m_lra.group(1)) if m_lra else None,
        TP=float(m_tp.group(1)) if m_tp else None,
    )


def within(value, expected, tol=None, tol_lo=None, tol_hi=None):
    if tol is not None:
        return abs(value - expected) <= tol
    return (expected - tol_lo) <= value <= (expected + tol_hi)


def main():
    rows = []
    for case in CASES:
        fp = path(case["file"])
        if not os.path.exists(fp):
            rows.append((case["case"], case["file"], "MISSING", "", "", "", ""))
            continue

        ours, err = run_loudness_ref(fp)
        if ours is None:
            rows.append((case["case"], case["file"], f"ERROR: {err}", "", "", "", ""))
            continue

        metric = case["metric"]
        key = {"I": "I", "LRA": "LRA", "TP": "TP", "maxS": "maxS", "maxM": "maxM"}[metric]
        ours_val = ours[key]

        # ffmpeg's -af ebur128 log doesn't expose a per-file running max of M/S (only libebur128's
        # API, polled the same way our own engine does — see ebur128_ref.c) — so maxM/maxS only
        # get the libebur128 cross-check, not the ffmpeg one.
        ebu, ffm = {}, {}
        try:
            ebu = run_ebur128_ref(fp)
            if metric in ("I", "LRA", "TP"):
                ffm = run_ffmpeg_summary(fp)
        except Exception:  # noqa: BLE001 - best-effort cross-check, don't abort the run
            ebu, ffm = {}, {}

        if metric == "TP":
            passed = within(ours_val, case["expected"], tol_lo=case["tol_lo"], tol_hi=case["tol_hi"])
        else:
            passed = within(ours_val, case["expected"], tol=case["tol"])

        rows.append((
            case["case"], case["file"],
            "PASS" if passed else "FAIL",
            f"{ours_val:.3f}",
            f"{ebu.get(key, float('nan')):.3f}" if ebu else "n/a",
            f"{ffm.get(key, float('nan')):.3f}" if ffm.get(key) is not None else "n/a",
            f"expected {case['expected']}",
        ))

    header = ("case", "file", "result", "ours", "libebur128", "ffmpeg", "expected")
    widths = [max(len(str(r[i])) for r in ([header] + rows)) for i in range(len(header))]
    def fmt(row):
        return "  ".join(str(v).ljust(w) for v, w in zip(row, widths))
    print(fmt(header))
    print(fmt(["-" * w for w in widths]))
    for r in rows:
        print(fmt(r))

    fails = [r for r in rows if r[2] not in ("PASS",)]
    print(f"\n{len(rows) - len(fails)}/{len(rows)} passed")
    if fails:
        print(f"{len(fails)} did not pass (FAIL, MISSING, or ERROR) — see above")
        sys.exit(1)


if __name__ == "__main__":
    main()
