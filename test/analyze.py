#!/usr/bin/env python3
"""Measure rendered WAVs for sama's regression suite.

sama is a drone instrument, so most of what is worth testing about a render is
spectral and temporal: how loud it is, whether it sits in tune, how bright it
is, and whether it moves.  This module turns a WAV file (or an in-memory float
array) into those numbers.

It is deliberately standalone: Python's standard library plus numpy, no scipy
and no project imports, so CI only needs ``pip install numpy``.

Public API
----------
``load_wav(path)``
    Read a PCM WAV -> ``(float64 samples in -1..1, sample rate)``.
``metrics(path_or_array, sr=None)``
    Level, tuning, spectrum and windowed level/pitch for one signal.
``track_modulation(path_or_array, sr=None)``
    Pitch span and amplitude-modulation depth over 0.5-2.0 s.
``compare(a, b, sr=None)``
    Relative difference per metric between two signals.

CLI
---
::

    python3 test/analyze.py FILE.wav [--json] [--track]
    python3 test/analyze.py --selftest

``--json`` prints every metric as JSON; ``--track`` adds the modulation
measurement.  ``--selftest`` synthesizes a 440 Hz sine and checks the analyzer
against known values.

Conventions
-----------
* Times are seconds, frequencies hertz, levels linear (full scale = 1.0).
* The steady-state window is 1.0-2.0 s when the file is long enough; shorter
  files fall back to a sensible chunk (see ``_analysis_bounds``).
* "Silence" means RMS < ``SILENT_RMS`` (about -100 dBFS).
* ``f0_track`` is the only place a NaN can appear: silent or unpitched windows.
* ``metrics`` accepts either a path or an already-loaded mono float array.  An
  array must already be scaled to -1..1 and needs ``sr``.
"""

import argparse
import json
import math
import os
import sys
import tempfile
import wave

import numpy as np

__all__ = ["load_wav", "metrics", "track_modulation", "compare", "main"]

# ---------------------------------------------------------------------------
# tuning constants
# ---------------------------------------------------------------------------

F0_MIN = 50.0            # Hz - lowest fundamental we search for
F0_MAX = 2000.0          # Hz - highest fundamental we search for
SILENT_RMS = 1e-5        # ~-100 dBFS; below this a window is treated as silence

# Spectral centroid ignores bins quieter than this relative to the strongest
# bin (-100 dB).  Quantization hash lives below that and would otherwise drag
# the centroid upwards, because there are thousands of noise bins and only a
# handful of signal bins.
CENTROID_FLOOR = 1e-5

ANALYSIS_START = 1.0     # s - start of the steady-state window
ANALYSIS_END = 2.0       # s - end of the steady-state window
MIN_ANALYSIS = 0.25      # s - shortest acceptable steady-state window

TRACK_WINDOW = 0.050     # s - pitch/level tracking window
TRACK_HOP = 0.010        # s - pitch/level tracking hop

MOD_START = 0.5          # s - start of the modulation measurement span
MOD_END = 2.0            # s - end of the modulation measurement span

# Energy bands, in hertz: low, mid, high, air.
BAND_EDGES = ((0.0, 200.0),
              (200.0, 2000.0),
              (2000.0, 8000.0),
              (8000.0, 16000.0))

# Decimal places used when printing JSON, so output is readable and stable.
_JSON_DIGITS = {
    "peak": 4,
    "rms": 4,
    "crest": 3,
    "f0": 2,
    "centroid": 2,
    "harmonic_ratio": 4,
    "flatness": 4,
    "low_energy": 4,
    "mid_energy": 4,
    "high_energy": 4,
    "air_energy": 4,
}

_METRIC_KEYS = ("peak", "rms", "crest", "zc", "f0", "centroid",
                "harmonic_ratio", "flatness", "low_energy", "mid_energy",
                "high_energy", "air_energy", "rms_windows", "f0_track")


# ---------------------------------------------------------------------------
# WAV decoding
# ---------------------------------------------------------------------------

def load_wav(path):
    """Read a PCM WAV file and return ``(samples, sample_rate)``.

    ``samples`` is a one-dimensional ``np.float64`` array in -1..1.  Stereo and
    other multi-channel files are averaged to mono.  8, 16, 24 and 32 bit
    integer PCM are supported, which covers everything the sama host harness
    writes.

    Raises ``ValueError`` for unsupported or malformed files.
    """
    try:
        with wave.open(os.fspath(path), "rb") as wav:
            channels = wav.getnchannels()
            width = wav.getsampwidth()
            rate = wav.getframerate()
            compression = wav.getcomptype()
            if compression != "NONE":
                raise ValueError("load_wav: %s uses unsupported compression %r"
                                 % (path, compression))
            raw = wav.readframes(wav.getnframes())
    except wave.Error as exc:
        raise ValueError("load_wav: %s is not a readable PCM WAV (%s)"
                         % (path, exc)) from exc

    if channels < 1 or rate <= 0:
        raise ValueError("load_wav: %s has an invalid header (channels=%r, "
                         "rate=%r)" % (path, channels, rate))

    # Drop a trailing partial frame; truncated files are common enough that a
    # hard error would be unhelpful.
    frame_size = width * channels
    raw = raw[:len(raw) - (len(raw) % frame_size)]

    if width == 1:
        # 8-bit WAV is unsigned with a 128 offset.
        samples = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64)
                   - 128.0) / 128.0
    elif width == 2:
        samples = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif width == 3:
        # 24-bit samples are packed; assemble and sign-extend them manually.
        packed = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        values = packed[:, 0] | (packed[:, 1] << 8) | (packed[:, 2] << 16)
        samples = ((values << 8) >> 8).astype(np.float64) / 8388608.0
    elif width == 4:
        samples = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError("load_wav: %s has unsupported sample width %d bytes"
                         % (path, width))

    if channels > 1:
        samples = samples.reshape(-1, channels).mean(axis=1)

    return np.ascontiguousarray(samples, dtype=np.float64), int(rate)


def _as_signal(source, sr):
    """Normalize a path or array into ``(float64 mono samples, sample rate)``."""
    if isinstance(source, (str, bytes, os.PathLike)):
        return load_wav(source)

    samples = np.asarray(source, dtype=np.float64)
    if samples.ndim != 1:
        raise ValueError("metrics: array input must be mono (1-D), got shape %r"
                         % (samples.shape,))
    if sr is None:
        raise ValueError("metrics: sr is required when passing samples "
                         "instead of a path")
    sr = int(sr)
    if sr <= 0:
        raise ValueError("metrics: sample rate must be positive, got %r" % (sr,))
    # A single bad sample should not poison every metric.
    if not np.all(np.isfinite(samples)):
        samples = np.nan_to_num(samples, nan=0.0, posinf=0.0, neginf=0.0)
    return samples, sr


# ---------------------------------------------------------------------------
# small numeric helpers
# ---------------------------------------------------------------------------

def _next_fast_len(n):
    """Smallest power of two >= n (numpy FFTs are fastest at these sizes)."""
    if n <= 1:
        return 1
    return 1 << int(math.ceil(math.log2(n)))


def _rms(x):
    """Root-mean-square of an array, 0.0 when empty."""
    if x.size == 0:
        return 0.0
    return float(np.sqrt(np.dot(x, x) / x.size))


def _analysis_bounds(n, sr):
    """Sample range of the steady-state analysis window, clamped to the file.

    Normally that is exactly 1.0-2.0 s.  A file that ends inside that span uses
    the tail it has; a file shorter than 1 s uses all of it.  The fallback
    keeps the window at least ``MIN_ANALYSIS`` long where possible, because a
    handful of samples is not enough to measure a spectrum.
    """
    start = int(round(ANALYSIS_START * sr))
    end = min(n, int(round(ANALYSIS_END * sr)))
    minimum = max(1, int(round(MIN_ANALYSIS * sr)))

    if end - start >= minimum:
        return start, end
    if n <= int(round(ANALYSIS_START * sr)):
        return 0, n
    # Not enough material at 1.0-2.0 s: analyse the last full second instead.
    return max(0, n - int(round(1.0 * sr))), n


def _spectrum(seg, sr):
    """Hann-windowed magnitude and power spectra, plus bin frequencies."""
    n = seg.size
    if n == 0:
        return np.zeros(0), np.zeros(0), np.zeros(0)
    window = np.hanning(n) if n > 1 else np.ones(n)
    spec = np.fft.rfft(seg * window)
    magnitude = np.abs(spec)
    power = magnitude * magnitude
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    return freqs, magnitude, power


def _estimate_f0(seg, sr):
    """Fundamental frequency of one frame, or 0.0 if it has no periodicity.

    Normalized autocorrelation via FFT (zero padded, so no circular wrap), with
    parabolic interpolation around the chosen peak for sub-bin accuracy.

    Simply taking the global maximum is prone to octave-down errors: for many
    waveforms the peak one or more periods out is a hair stronger than the true
    period.  We therefore prefer the earliest local peak that is within 10% of
    the strongest one, which keeps the estimate on the fundamental.
    """
    n = seg.size
    if n < 8:
        return 0.0

    seg = seg - float(seg.mean())
    energy = float(np.dot(seg, seg))
    if energy <= 0.0:
        return 0.0

    nfft = _next_fast_len(2 * n)
    spec = np.fft.rfft(seg, nfft)
    ac = np.fft.irfft(spec * np.conj(spec), nfft)[:n]
    ac /= ac[0]                      # ac[0] == 1 after normalization

    lo = max(2, int(math.floor(sr / F0_MAX)))
    hi = min(n - 2, int(math.ceil(sr / F0_MIN)))
    if hi - lo < 2:
        return 0.0

    region = ac[lo:hi + 1]
    peaks = np.flatnonzero((region[1:-1] > region[:-2]) &
                           (region[1:-1] >= region[2:])) + 1
    if peaks.size:
        strongest = int(peaks[int(np.argmax(region[peaks]))])
        cutoff = 0.9 * float(region[strongest])
        early = peaks[region[peaks] >= cutoff]
        chosen = int(early[0]) if early.size else strongest
    else:
        chosen = int(np.argmax(region))
    lag = lo + chosen

    # Parabolic interpolation through the peak and its two neighbours.
    y0, y1, y2 = float(ac[lag - 1]), float(ac[lag]), float(ac[lag + 1])
    denom = y0 - 2.0 * y1 + y2
    offset = 0.0
    if denom < 0.0:
        offset = 0.5 * (y0 - y2) / denom
        offset = max(-0.5, min(0.5, offset))
    return float(sr / (lag + offset))


def _harmonic_ratio(freqs, power, f0):
    """Fraction of spectral power within 1% (at least 3 Hz) of f0 harmonics.

    A bin is counted once even when neighbouring harmonic masks overlap, which
    is why the mask is OR-ed together instead of summing per harmonic.
    """
    total = float(power.sum())
    if f0 <= 0.0 or total <= 0.0 or freqs.size == 0:
        return 0.0

    nyquist = float(freqs[-1])
    mask = np.zeros(freqs.shape, dtype=bool)
    harmonic = 1
    while harmonic * f0 <= nyquist:
        center = harmonic * f0
        tolerance = max(3.0, 0.01 * center)
        mask |= np.abs(freqs - center) <= tolerance
        harmonic += 1
    return min(1.0, float(power[mask].sum()) / total)


def _spectral_flatness(power):
    """Geometric mean / arithmetic mean of the power spectrum, in 0..1."""
    if power.size == 0:
        return 0.0
    mean = float(power.mean())
    if mean <= 0.0:
        return 0.0
    # A floor relative to the mean keeps log() finite without lifting real
    # energy: a pure tone still lands near zero.
    floor = mean * 1e-12
    geometric = float(np.exp(np.mean(np.log(power + floor))))
    return max(0.0, min(1.0, geometric / mean))


def _band_fractions(freqs, power):
    """Fraction of spectral power in each of the four energy bands."""
    total = float(power.sum())
    if total <= 0.0:
        return 0.0, 0.0, 0.0, 0.0
    fractions = []
    for low, high in BAND_EDGES:
        mask = (freqs >= low) & (freqs < high)
        fractions.append(float(power[mask].sum()) / total)
    return tuple(fractions)


# ---------------------------------------------------------------------------
# framing and tracking
# ---------------------------------------------------------------------------

def _rms_windows(x, sr):
    """RMS of consecutive 50 ms windows (the last partial window is dropped)."""
    if x.size == 0:
        return []
    win = max(1, int(round(TRACK_WINDOW * sr)))
    if x.size < win:
        return [_rms(x)]
    count = x.size // win
    return [_rms(x[i * win:(i + 1) * win]) for i in range(count)]


def _f0_track(x, sr):
    """Pitch and level every 10 ms, using a 50 ms window.

    Returns a list of ``(time, f0, rms)``: ``time`` is the window center in
    seconds, ``f0`` is hertz or NaN for silent/unpitched windows.
    """
    n = x.size
    if n == 0:
        return []

    win = max(1, int(round(TRACK_WINDOW * sr)))
    hop = max(1, int(round(TRACK_HOP * sr)))
    starts = [0] if n <= win else range(0, n - win + 1, hop)

    track = []
    for start in starts:
        seg = x[start:start + win]
        level = _rms(seg)
        if level < SILENT_RMS:
            f0 = float("nan")
        else:
            f0 = _estimate_f0(seg, sr)
            if f0 <= 0.0:
                f0 = float("nan")
        center = (start + 0.5 * seg.size) / sr
        track.append((center, f0, level))
    return track


# ---------------------------------------------------------------------------
# public measurements
# ---------------------------------------------------------------------------

def metrics(path_or_array, sr=None):
    """Measure one signal.

    ``path_or_array`` is a WAV path or a mono ``np.float64`` array; ``sr`` is
    required for arrays and ignored for paths.  The returned dict always has
    these keys:

    ``peak``
        Largest absolute sample, 0..1.
    ``rms``
        Root-mean-square over the whole file, 0..1.
    ``crest``
        ``peak / rms`` (1.414 for a sine); 0 when silent.
    ``zc``
        Number of sign changes over the whole file.
    ``f0``
        Autocorrelation fundamental of the steady-state window in hertz;
        0 when that window is silent.
    ``centroid``
        Magnitude-weighted mean frequency of the steady-state window (Hz).
    ``harmonic_ratio``
        Fraction of the window's power within 1% (minimum 3 Hz) of harmonics
        of ``f0``, 0..1.
    ``flatness``
        Spectral flatness (geometric / arithmetic mean of power), 0..1.
    ``low_energy`` / ``mid_energy`` / ``high_energy`` / ``air_energy``
        Fraction of power below 200 Hz, in 200-2000 Hz, in 2000-8000 Hz and
        in 8000-16000 Hz respectively.
    ``rms_windows``
        List of RMS values for consecutive 50 ms windows.
    ``f0_track``
        List of ``(time, f0, rms)`` at 50 ms windows / 10 ms hop; ``f0`` is
        NaN for silent windows and ``time`` is the window center.
    """
    x, sr = _as_signal(path_or_array, sr)

    result = {key: [] if key in ("rms_windows", "f0_track") else 0.0
              for key in _METRIC_KEYS}
    result["zc"] = 0

    n = x.size
    if n == 0:
        return result

    # Whole-file level and zero crossings.
    peak = float(np.max(np.abs(x)))
    rms = _rms(x)
    result["peak"] = peak
    result["rms"] = rms
    result["crest"] = peak / rms if rms > 0.0 else 0.0
    result["zc"] = int(np.count_nonzero((x[:-1] < 0.0) != (x[1:] < 0.0)))
    result["rms_windows"] = _rms_windows(x, sr)
    result["f0_track"] = _f0_track(x, sr)

    # Steady-state window: pitch, spectrum, bands.
    start, end = _analysis_bounds(n, sr)
    seg = x[start:end]
    if seg.size >= 2:
        seg = seg - float(seg.mean())
        if _rms(seg) >= SILENT_RMS:
            f0 = _estimate_f0(seg, sr)
            result["f0"] = f0

            freqs, magnitude, power = _spectrum(seg, sr)
            if float(magnitude.sum()) > 0.0:
                floor = float(magnitude.max()) * CENTROID_FLOOR
                keep = magnitude >= floor
                kept = float(magnitude[keep].sum())
                if kept > 0.0:
                    result["centroid"] = float(
                        np.dot(freqs[keep], magnitude[keep]) / kept)
            if float(power.sum()) > 0.0:
                result["harmonic_ratio"] = _harmonic_ratio(freqs, power, f0)
                result["flatness"] = _spectral_flatness(power)
                (result["low_energy"], result["mid_energy"],
                 result["high_energy"], result["air_energy"]) = \
                    _band_fractions(freqs, power)

    return result


def track_modulation(path_or_array, sr=None):
    """Measure pitch movement and amplitude modulation between 0.5 and 2.0 s.

    Returns a dict with:

    ``pitch_span_cents``
        ``1200 * log2(max_f0 / min_f0)`` over the span; 0 when fewer than two
        pitched windows are available.
    ``am_depth``
        ``(max_rms - min_rms) / (max_rms + min_rms)`` over the span; 0 when
        there is no level at all.
    ``times``, ``f0``, ``rms``
        The raw per-window arrays the two numbers were computed from, as
        ``np.float64`` arrays.  ``f0`` is NaN in silent windows.
    """
    x, sr = _as_signal(path_or_array, sr)
    track = _f0_track(x, sr)
    chosen = [row for row in track if MOD_START <= row[0] <= MOD_END]

    times = np.array([row[0] for row in chosen], dtype=np.float64)
    f0 = np.array([row[1] for row in chosen], dtype=np.float64)
    rms = np.array([row[2] for row in chosen], dtype=np.float64)

    usable = f0[np.isfinite(f0) & (f0 > 0.0)]
    if usable.size >= 2:
        low = float(usable.min())
        high = float(usable.max())
        pitch_span = 1200.0 * math.log2(high / low) if low > 0.0 else 0.0
    else:
        pitch_span = 0.0

    if rms.size:
        high_rms = float(rms.max())
        low_rms = float(rms.min())
        total = high_rms + low_rms
        am_depth = (high_rms - low_rms) / total if total > 0.0 else 0.0
    else:
        am_depth = 0.0

    return {"pitch_span_cents": pitch_span,
            "am_depth": am_depth,
            "times": times,
            "f0": f0,
            "rms": rms}


def compare(a, b, sr=None):
    """Relative differences between two signals, per metric.

    ``a`` and ``b`` are WAV paths or float arrays (``sr`` is required for
    arrays).  The result maps ``centroid``, ``harmonic_ratio``, ``rms`` and
    ``f0`` to ``abs(va - vb) / max(abs(va), abs(vb))``: 0 when identical, 1
    when one of the two is zero.  ``0.0`` also means "both silent".
    """
    left = metrics(a, sr)
    right = metrics(b, sr)

    differences = {}
    for key in ("centroid", "harmonic_ratio", "rms", "f0"):
        va = float(left[key])
        vb = float(right[key])
        scale = max(abs(va), abs(vb))
        differences[key] = abs(va - vb) / scale if scale > 0.0 else 0.0
    return differences


# ---------------------------------------------------------------------------
# command line
# ---------------------------------------------------------------------------

def _human_line(name, measured):
    """One compact, scannable line of the headline numbers."""
    return ("%s  peak=%.3f rms=%.3f crest=%.2f f0=%.1fHz centroid=%.0fHz "
            "zc=%d harm=%.2f flat=%.4f low=%.2f mid=%.2f high=%.2f air=%.2f"
            % (name, measured["peak"], measured["rms"], measured["crest"],
               measured["f0"], measured["centroid"], measured["zc"],
               measured["harmonic_ratio"], measured["flatness"],
               measured["low_energy"], measured["mid_energy"],
               measured["high_energy"], measured["air_energy"]))


def _modulation_line(modulation):
    """One compact line for the 0.5-2.0 s movement measurement."""
    return ("modulation  pitch_span=%.1f cents  am_depth=%.3f  "
            "(0.5-2.0 s, %d windows)"
            % (modulation["pitch_span_cents"], modulation["am_depth"],
               modulation["times"].size))


def _json_metrics(measured):
    """Round the metrics dict into plain, JSON-safe Python objects."""
    output = {}
    for key in _METRIC_KEYS:
        value = measured[key]
        if key == "zc":
            output[key] = int(value)
        elif key == "rms_windows":
            output[key] = [round(float(v), 5) for v in value]
        elif key == "f0_track":
            output[key] = [
                [round(float(time), 3),
                 round(float(f0), 2) if math.isfinite(f0) else None,
                 round(float(level), 5)]
                for time, f0, level in value
            ]
        else:
            output[key] = round(float(value), _JSON_DIGITS[key])
    return output


def _json_modulation(modulation):
    """Round the modulation dict into plain, JSON-safe Python objects."""
    return {
        "pitch_span_cents": round(float(modulation["pitch_span_cents"]), 2),
        "am_depth": round(float(modulation["am_depth"]), 4),
        "times": [round(float(v), 3) for v in modulation["times"]],
        "f0": [round(float(v), 2) if math.isfinite(v) else None
               for v in modulation["f0"]],
        "rms": [round(float(v), 5) for v in modulation["rms"]],
    }


def _selftest():
    """Synthesize a 440 Hz sine and check the analyzer against known values."""
    rate = 44100
    duration = 3.0
    frequency = 440.0
    amplitude = 0.5

    time = np.arange(int(rate * duration), dtype=np.float64) / rate
    tone = amplitude * np.sin(2.0 * np.pi * frequency * time)
    pcm = np.round(tone * 32767.0).astype("<i2")

    print("selftest: %.1f s, %.1f Hz sine at %d Hz, 16-bit mono"
          % (duration, frequency, rate))

    with tempfile.TemporaryDirectory(prefix="sama-analyze-") as tmp:
        path = os.path.join(tmp, "sine-440.wav")
        with wave.open(path, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(pcm.tobytes())

        samples, sample_rate = load_wav(path)
        measured = metrics(path)
        movement = track_modulation(path)

    results = []

    def check(name, ok, detail):
        results.append(bool(ok))
        print(("  ok   " if ok else "  FAIL ") + "%-15s %s" % (name, detail))

    check("sample rate", sample_rate == rate, "%d Hz" % sample_rate)
    check("length", samples.size == pcm.size, "%d samples" % samples.size)
    check("f0", abs(measured["f0"] - frequency) < 0.5,
          "%.2f Hz" % measured["f0"])
    check("centroid", abs(measured["centroid"] - frequency) < 5.0,
          "%.2f Hz" % measured["centroid"])
    check("harmonic_ratio", measured["harmonic_ratio"] > 0.9,
          "%.4f" % measured["harmonic_ratio"])
    check("crest", abs(measured["crest"] - math.sqrt(2.0)) < 0.01,
          "%.4f" % measured["crest"])
    check("rms", abs(measured["rms"] - amplitude / math.sqrt(2.0)) < 0.005,
          "%.4f" % measured["rms"])
    check("zero crossings", abs(measured["zc"] - 2.0 * frequency * duration) <= 3,
          "%d" % measured["zc"])
    # 50 ms autocorrelation frames have a few cents of inherent jitter, so
    # "steady" means "not moving by more than a tenth of a semitone".
    check("pitch steady", movement["pitch_span_cents"] < 10.0,
          "%.3f cents" % movement["pitch_span_cents"])
    check("am steady", movement["am_depth"] < 0.01,
          "%.4f" % movement["am_depth"])

    passed = all(results)
    print("selftest " + ("passed" if passed else "FAILED"))
    return 0 if passed else 1


def main(argv=None):
    """Command line entry point; returns a process exit code."""
    parser = argparse.ArgumentParser(
        prog="analyze.py",
        description="Measure level, tuning, brightness and movement of a WAV.",
        epilog="example: python3 test/analyze.py render.wav --track")
    parser.add_argument("file", nargs="?", metavar="FILE.wav",
                        help="WAV file to analyze")
    parser.add_argument("--json", action="store_true",
                        help="print all metrics as JSON")
    parser.add_argument("--track", action="store_true",
                        help="add the 0.5-2.0 s modulation measurement")
    parser.add_argument("--selftest", action="store_true",
                        help="run a synthetic self-test and exit")
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()
    if not args.file:
        parser.error("FILE.wav is required unless --selftest is given")

    try:
        if args.json:
            payload = _json_metrics(metrics(args.file))
            if args.track:
                payload["modulation"] = _json_modulation(
                    track_modulation(args.file))
            print(json.dumps(payload, indent=2, sort_keys=True,
                             allow_nan=False))
        else:
            print(_human_line(os.path.basename(args.file),
                              metrics(args.file)))
            if args.track:
                print(_modulation_line(track_modulation(args.file)))
    except (OSError, ValueError) as exc:
        print("analyze: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
