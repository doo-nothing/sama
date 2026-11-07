#!/usr/bin/env python3
"""Regression tests for sama 1.0.

Builds the host harness (which compiles the real sama.c), renders a set of parameter
configurations, and checks the instrument still behaves:

  * it sounds, it is not clipping, and it keeps sounding (the drone);
  * one voice has headroom and a triangle keeps its third harmonic;
  * six voices stay bounded;
  * the pitch and cross modulation destinations are audible;
  * the warp is at f/2, not f/8;
  * the tuned room is in tune;
  * the pitch scale quantizer changes the melody;
  * freeze sustains the room; scenes change it; the face data survives save/load;
  * every one of the twenty deep controls and the four macros changes the sound;
  * an extreme six-voice setting is stable.

Requires numpy (pip install numpy) for test/analyze.py.
Exit code 0 when everything holds, 1 with a list of failures. Run from anywhere.
"""
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.environ.get("OPUS_KIT", "")
HOST = os.path.join(HERE, "host")
sys.path.insert(0, HERE)
import analyze  # noqa: E402

# the default sound: the choir scene + its macro values
BASE = [45, 70, 92, 50, 35, 25, 35, 70, 35, 40, 25, 45, 30, 15, 45, 30, 35, 50, 35, 5]
BASE_ENV = {"SAMA_MACRO": "55,45,60,55"}

CLEAN = [0, 60, 0, 99, 0, 0, 0, 0, 0, 50, 0, 30, 0, 0, 0, 0, 40, 50, 0, 0]
NEUTRAL = {"SAMA_KEYTRACK": "0", "SAMA_DIFFUSE": "0", "SAMA_WOW": "0",
           "SAMA_MACRO": "50,50,50,50"}

PITCH = [0, 60, 0, 99, 0, 0, 0, 0, 0, 50, 0, 30, 50, 0, 99, 74, 40, 50, 0, 0]
CROSS = [0, 60, 0, 99, 0, 0, 0, 0, 50, 50, 0, 30, 50, 0, 99, 99, 40, 50, 0, 0]
CROSS0 = [0, 60, 0, 99, 0, 0, 0, 0, 50, 50, 0, 30, 0, 0, 0, 0, 40, 50, 0, 0]
TUNE = [0, 0, 95, 90, 60, 0, 0, 99, 0, 50, 0, 30, 0, 0, 0, 0, 50, 50, 0, 0]
WARP = [0, 99, 0, 99, 0, 99, 0, 99, 0, 50, 0, 30, 0, 0, 0, 0, 40, 50, 0, 0]

NAMES = [["fold", "time", "decay", "filter"],
         ["tone", "warp", "drift", "blend"],
         ["cross", "polar", "drive", "res"],
         ["rate", "shape", "depth", "dest"],
         ["core", "bias", "growl", "fuzz"]]
MACRO_NAMES = ["body", "pressure", "motion", "space"]


def build():
    inc = ["-I" + KIT] if KIT else []
    cmd = ["cc", "-O2", "-w"] + inc + ["-o", HOST, os.path.join(HERE, "host.c"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-3000:])
        sys.exit("regression: the host harness failed to compile")


def run(params, env=None, name="reg"):
    path = "/tmp/sama_%s.wav" % name
    e = os.environ.copy()
    e.update(env or {})
    r = subprocess.run([HOST, path] + [str(p) for p in params],
                       capture_output=True, text=True, env=e)
    if r.returncode != 0:
        raise RuntimeError("host failed: " + r.stderr[-800:])
    out = {"_wav": path}
    for line in r.stdout.strip().splitlines():
        p = line.split()
        for i in range(0, len(p), 2):
            try:
                out[p[i]] = float(p[i + 1])
            except (ValueError, IndexError):
                out[p[i]] = p[i + 1]
    return out


def wave_diff(a, b):
    """Relative difference between two renders: 0 when a control does nothing."""
    import array
    da, db = array.array("h"), array.array("h")
    for path, dst in ((a, da), (b, db)):
        with open(path, "rb") as f:
            f.seek(44)
            dst.frombytes(f.read())
    n = min(len(da), len(db))
    if n == 0:
        return 0.0
    num = sum((da[i] - db[i]) ** 2 for i in range(n))
    den = sum(da[i] ** 2 for i in range(n)) + 1.0
    return (num / den) ** 0.5


def spectrum(path):
    x, sr = analyze.load_wav(path)
    seg = x[int(1.0 * sr):int(2.0 * sr)]
    seg = seg - seg.mean()
    win = np.hanning(len(seg))
    mag = np.abs(np.fft.rfft(seg * win))
    freq = np.fft.rfftfreq(len(seg), 1.0 / sr)
    return freq, mag


def peak_in(freq, mag, lo, hi):
    m = (freq >= lo) & (freq <= hi)
    if not np.any(m):
        return 0.0, 0.0
    i = int(np.argmax(mag[m]))
    return float(freq[m][i]), float(mag[m][i])


def main():
    build()
    fails = []

    def check(name, ok, detail):
        print(("  ok   " if ok else "  FAIL ") + name + "  " + detail)
        if not ok:
            fails.append(name)

    print("baseline:")
    base = run(BASE, BASE_ENV, "base")
    bm = analyze.metrics(base["_wav"])
    check("audible", 8000 <= base["peak"] <= 30000, "peak=%.0f" % base["peak"])
    check("level", 1500 <= base["rms1"] <= 12000, "rms1=%.0f" % base["rms1"])
    check("drone tail", base["rms5"] >= 0.01 * base["rms1"],
          "rms5=%.0f rms1=%.0f" % (base["rms5"], base["rms1"]))
    check("no core clipping", base["clip"] == 0, "clip=%.0f" % base["clip"])
    check("in tune-ish", 0.9 <= bm["f0"] / 440.0 <= 1.1, "f0=%.1f Hz" % bm["f0"])

    print("\ngain / headroom:")
    tri = run(CLEAN, NEUTRAL, "tri")
    tm = analyze.metrics(tri["_wav"])
    check("triangle has headroom", tm["peak"] <= 0.70, "peak=%.3f" % tm["peak"])
    check("triangle crest", tm["crest"] >= 3.2, "crest=%.2f" % tm["crest"])
    check("triangle in tune", abs(tm["f0"] - 440.0) < 8.0, "f0=%.1f Hz" % tm["f0"])
    check("triangle harmonic", tm["harmonic_ratio"] >= 0.90,
          "harmonic=%.2f" % tm["harmonic_ratio"])

    six = run(BASE, dict(BASE_ENV, SAMA_VOICES="6"), "six")
    check("six voices bounded", six["peak"] <= 32767, "peak=%.0f" % six["peak"])
    check("six voices no hard clip", six["clip"] == 0, "clip=%.0f" % six["clip"])
    check("six voices louder", six["rms1"] >= 1.05 * base["rms1"],
          "rms1=%.0f vs %.0f" % (six["rms1"], base["rms1"]))

    print("\nmodulation:")
    p = run(PITCH, NEUTRAL, "pitch")
    pm = analyze.track_modulation(p["_wav"])
    check("pitch mod audible", pm["pitch_span_cents"] >= 200.0,
          "span=%.1f cents" % pm["pitch_span_cents"])
    ps = run(PITCH, dict(NEUTRAL, SAMA_SCALE="1"), "pitch_scale")
    sm = analyze.track_modulation(ps["_wav"])
    check("scale changes the melody", wave_diff(p["_wav"], ps["_wav"]) >= 0.01,
          "waveform difference %.3f" % wave_diff(p["_wav"], ps["_wav"]))
    check("scale keeps pitch movement", sm["pitch_span_cents"] >= 100.0,
          "span=%.1f cents" % sm["pitch_span_cents"])

    c1 = run(CROSS, NEUTRAL, "cross")
    c0 = run(CROSS0, NEUTRAL, "cross0")
    cm1 = analyze.track_modulation(c1["_wav"])
    cm0 = analyze.track_modulation(c0["_wav"])
    check("cross mod audible", cm1["am_depth"] >= 0.18,
          "am=%.3f" % cm1["am_depth"])
    check("cross beats the baseline", cm1["am_depth"] >= 1.3 * max(cm0["am_depth"], 0.01),
          "am=%.3f vs %.3f" % (cm1["am_depth"], cm0["am_depth"]))

    print("\nwarp and tune:")
    w = run(WARP, NEUTRAL, "warp")
    freq, mag = spectrum(w["_wav"])
    f2, a2 = peak_in(freq, mag, 180, 260)
    f8, a8 = peak_in(freq, mag, 40, 80)
    check("warp is f/2", a2 > 2.0 * a8,
          "f2=%.1f Hz a=%.0f; f8=%.1f Hz a=%.0f" % (f2, a2, f8, a8))

    t = run(TUNE, dict(NEUTRAL, SAMA_TUNE="1"), "tune")
    tm2 = analyze.metrics(t["_wav"])
    check("tuned room is in tune", abs(tm2["f0"] - 440.0) < 8.0,
          "f0=%.1f Hz" % tm2["f0"])
    check("tuned room is harmonic", tm2["harmonic_ratio"] >= 0.85,
          "harmonic=%.2f" % tm2["harmonic_ratio"])
    check("tuned room rings", t["rms5"] >= 0.01 * max(t["rms1"], 1.0),
          "rms5=%.0f rms1=%.0f" % (t["rms5"], t["rms1"]))

    print("\nperformance:")
    fr = run(BASE, dict(BASE_ENV, SAMA_FREEZE_AT="2.5"), "freeze")
    check("freeze sustains", fr["rms5"] >= 2.0 * max(base["rms5"], 1.0),
          "freeze rms5=%.0f vs %.0f" % (fr["rms5"], base["rms5"]))
    check("freeze is a drone", fr["rms5"] >= 0.10 * max(fr["rms1"], 1.0),
          "rms5=%.0f rms1=%.0f" % (fr["rms5"], fr["rms1"]))

    sc = run(BASE, dict(BASE_ENV, SAMA_SCENE_AT="1.0"), "scene")
    check("scene changes the sound", wave_diff(base["_wav"], sc["_wav"]) >= 0.02,
          "waveform difference %.3f" % wave_diff(base["_wav"], sc["_wav"]))

    per = run(BASE, dict(BASE_ENV, SAMA_PERSIST="1"), "persist")
    check("faces persist", per.get("persist") == 1, "persist=%s" % per.get("persist"))

    kt = run(BASE, dict(BASE_ENV, SAMA_KEYTEST="1"), "keytest")
    check("T4 on T2 passes to the OS", kt.get("keypage_t2") == 0,
          "keypage_t2=%s" % kt.get("keypage_t2"))
    check("T4 on T1 pages a face", kt.get("keypage_t1") == 1,
          "keypage_t1=%s" % kt.get("keypage_t1"))

    print("\ncontrols (each against its own 0 and 99):")
    for i, name in enumerate(sum(NAMES, [])):
        p0, p1 = list(BASE), list(BASE)
        p0[i] = 0
        p1[i] = 99
        a = run(p0, BASE_ENV, "c0")
        b = run(p1, BASE_ENV, "c1")
        d = wave_diff(a["_wav"], b["_wav"])
        check("control %02d %-7s" % (i, name), d >= 0.01,
              "waveform difference %.3f" % d)

    for i, name in enumerate(MACRO_NAMES):
        m0 = [50, 50, 50, 50]
        m1 = [50, 50, 50, 50]
        m0[i] = 0
        m1[i] = 99
        a = run(BASE, dict(BASE_ENV, SAMA_MACRO=",".join(map(str, m0))), "m0")
        b = run(BASE, dict(BASE_ENV, SAMA_MACRO=",".join(map(str, m1))), "m1")
        d = wave_diff(a["_wav"], b["_wav"])
        check("macro %-8s" % name, d >= 0.01, "waveform difference %.3f" % d)

    print("\nsettings:")
    d0 = run(BASE, dict(BASE_ENV, SAMA_DIFFUSE="0"), "d0")
    d1 = run(BASE, dict(BASE_ENV, SAMA_DIFFUSE="99"), "d1")
    check("diffusion changes the room", wave_diff(d0["_wav"], d1["_wav"]) >= 0.01,
          "waveform difference %.3f" % wave_diff(d0["_wav"], d1["_wav"]))
    w0 = run(BASE, dict(BASE_ENV, SAMA_WOW="0"), "w0")
    w1 = run(BASE, dict(BASE_ENV, SAMA_WOW="99"), "w1")
    check("tape wow moves the room", wave_diff(w0["_wav"], w1["_wav"]) >= 0.005,
          "waveform difference %.3f" % wave_diff(w0["_wav"], w1["_wav"]))
    low = run(BASE, dict(BASE_ENV, SAMA_CHORD="-24", SAMA_KEYTRACK="100"), "low")
    high = run(BASE, dict(BASE_ENV, SAMA_CHORD="24", SAMA_KEYTRACK="100"), "high")
    lm = analyze.metrics(low["_wav"])
    hm = analyze.metrics(high["_wav"])
    check("key tracking opens high notes", hm["centroid"] >= 1.2 * max(lm["centroid"], 1.0),
          "high=%.0f Hz low=%.0f Hz" % (hm["centroid"], lm["centroid"]))

    print("\nstability:")
    extreme = [99, 99, 99, 99, 99, 99, 99, 99, 99, 0, 99, 99, 99, 75, 99, 99, 99, 0, 99, 99]
    ex = run(extreme, {"SAMA_VOICES": "6", "SAMA_MACRO": "99,99,99,99"}, "extreme")
    check("extreme peak bounded", ex["peak"] <= 32767, "peak=%.0f" % ex["peak"])
    check("extreme still sounds", ex["rms1"] >= 100, "rms1=%.0f" % ex["rms1"])
    check("extreme not pinned", ex["clip"] <= 0.05 * 44100 * 8 / 2,
          "clipped=%.0f" % ex["clip"])

    print("\nanalyzer selftest:")
    r = subprocess.run([sys.executable, os.path.join(HERE, "analyze.py"), "--selftest"],
                       capture_output=True, text=True)
    check("analyzer selftest", r.returncode == 0, (r.stdout + r.stderr).strip().splitlines()[-1])

    print()
    if fails:
        print("%d regression(s) failed: %s" % (len(fails), ", ".join(fails)))
        return 1
    print("all regressions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
