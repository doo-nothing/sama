#!/usr/bin/env python3
"""Regression tests for sama.

Builds the host harness (which compiles the real sama.c), renders a set of parameter
configurations and asserts the instrument still does what it is supposed to do:

  * it sounds, it is not clipping, and it keeps sounding (the drone);
  * every one of the twenty controls still changes the render;
  * the loop decays when decay is low and sustains when it is high;
  * the oscillator is in tune;
  * a released, low-feedback configuration goes quiet instead of buzzing.

Exit code 0 when everything holds, 1 with a list of failures. Run from anywhere.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.environ.get("OPUS_KIT", "")
HOST = os.path.join(HERE, "host")

# the five faces, k99, in the order the engine takes them
BASE = [55, 61, 92, 54,     # niche: fold, time, decay, filter
        8, 40, 30, 78,      # space: absorb, warp, drift, blend
        35, 30, 10, 40,     # cross: cross, polar, drive, tone
        36, 15, 50, 30,     # mod:   rate, shape, depth, dest
        30, 30, 0, 9]       # core:  core, shape, fuzz, grit

NAMES = [["fold", "time", "decay", "filter"],
         ["absorb", "warp", "drift", "blend"],
         ["cross", "polar", "drive", "tone"],
         ["rate", "shape", "depth", "dest"],
         ["core", "shape", "fuzz", "grit"]]


def build():
    inc = ["-I" + KIT] if KIT else []
    cmd = ["cc", "-O2", "-w"] + inc + ["-o", HOST, os.path.join(HERE, "host.c"), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-3000:])
        sys.exit("regression: the host harness failed to compile")


def run(params, path="/tmp/sama_reg.wav"):
    r = subprocess.run([HOST, path] + [str(p) for p in params],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("host failed: " + r.stderr[-500:])
    out = {"_wav": path}
    for line in r.stdout.strip().splitlines():
        p = line.split()
        for i in range(0, len(p), 2):
            out[p[i]] = float(p[i + 1])
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


def main():
    build()
    fails = []
    base = run(BASE)

    def check(name, ok, detail):
        print(("  ok   " if ok else "  FAIL ") + name + "  " + detail)
        if not ok:
            fails.append(name)

    print("baseline:", " ".join("%s=%.0f" % (k, v) for k, v in base.items() if not k.startswith("_")))
    check("peak in range", 12000 <= base["peak"] <= 32767, "peak=%.0f" % base["peak"])
    check("audible", 2500 <= base["rms2"] <= 26000, "rms2=%.0f" % base["rms2"])
    check("tail after release", base["rms5"] >= 0.03 * base["rms2"],
          "rms5=%.0f vs rms2=%.0f" % (base["rms5"], base["rms2"]))
    clean = list(BASE); clean[0] = 0; clean[7] = 0; clean[16] = 0; clean[19] = 0
    c = run(clean)
    check("in tune", 0.9 <= c["zc"] / (440 * 2 * 2) <= 1.1, "zc=%.0f over 2 s" % c["zc"])

    # every control must move the sound
    print("\ncontrols (each against its own 0 and 99):")
    for i, name in enumerate(sum(NAMES, [])):
        p0 = list(BASE); p0[i] = 0
        p1 = list(BASE); p1[i] = 99
        a = run(p0, "/tmp/sama_a.wav")
        b = run(p1, "/tmp/sama_b.wav")
        d = wave_diff(a["_wav"], b["_wav"])
        check("control %02d %-7s" % (i, name), d >= 0.02, "waveform difference %.3f" % d)

    # the loop must decay when the tails are short, and ring when they are long
    print("\nloop:")
    quiet = list(BASE); quiet[2] = 0; quiet[4] = 99; quiet[7] = 99  # decay 0, absorb 99, all wet
    tail = run(quiet)
    loud = list(BASE); loud[2] = 99; loud[4] = 0
    ring = run(loud)
    check("decay shapes the tail", ring["rms5"] >= 3.0 * max(tail["rms5"], 1.0),
          "short=%.0f long=%.0f" % (tail["rms5"], ring["rms5"]))
    check("long tail still rings", ring["rms5"] >= 0.25 * ring["rms2"],
          "rms2=%.0f rms5=%.0f" % (ring["rms2"], ring["rms5"]))

    # the oscillator's cores must differ
    print("\ncores:")
    cores = {}
    for c in (5, 35, 60, 90):
        p = list(BASE); p[16] = c
        cores[c] = run(p)
    distinct = len({round(cores[c]["rms2"] / 50) for c in cores})
    check("four cores differ", distinct >= 3, "distinct levels %d/4" % distinct)

    print()
    if fails:
        print("%d regression(s) failed: %s" % (len(fails), ", ".join(fails)))
        return 1
    print("all regressions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
