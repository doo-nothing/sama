# sama

[![build](https://github.com/doo-nothing/sama/actions/workflows/build.yml/badge.svg)](https://github.com/doo-nothing/sama/actions/workflows/build.yml)

**An instrument of light and repetition — a drone synth for the original OP-1.**

One oscillator into one shared delay. The delay's filter, its loss and its feedback age the
sound; the note's own sub-harmonic warps the delay time; the loop moves the pitch and the
level of the source. Six voices sing into one room, and the room answers.

Built for [Opus 2](https://github.com/LizardMan245/opus) on the original OP-1 (firmware #246,
Blackfin BF524). Real-time, integer-only DSP. No samples, no libs, no floating point.

![niche](art/niche.png)

| | |
|---|---|
| ![space](art/space.png) | ![cross](art/cross.png) |
| ![mod](art/mod.png) | ![core](art/core.png) |

*(Not mockups: these are rendered from the plugin's own drawing code by `test/render.c` and
`test/render.py`, at the OP-1's true 320×160.)*

---

## What it is

Strega by Make Noise is a semi-modular drone instrument: an oscillator dripped into a lo-fi
delay, the delay's filter and loss inside the feedback loop, an agitation generator breathing
on the cutoff, and every path able to modulate every other. **sama** takes that architecture
and inverts its temperament — where Strega is a witch working alchemy, sama is a niche holding
a lamp. It is not affiliated with or endorsed by Make Noise; it is a homage.

- **One oscillator, one delay.** Every voice shares the delay line — polyphony is a choir in
  one room, not six rooms.
- **The loop is the instrument.** Filter, absorb, feedback, drive and the loop's own highpass
  all sit *inside* the regeneration path.
- **The sub-harmonic is a modulator, not a voice.** It bends the delay time (warp) and the
  oscillator's pitch, the way patching the sub-oscillator to Tonic does on the hardware.
- **The loop feeds back into the source.** Its level ducks or lifts the oscillator (cross),
  its envelope sags the pitch (drift) and colours the filter.

## Pages and faces

**T2 and T4 stay the system's pages** (envelope, LFO). Every sama control lives on **T1**,
where encoder turns are actually delivered: the four knobs are the OS's own, and a *face*
decides what they mean. **T3** pages to the previous face, **T4** to the next — the screen
stays on T1, and the system LFO keeps modulating the four knobs.

Each face keeps its own four values, so twenty parameters live behind four encoders.

| knob | niche | space | cross | mod | core |
|---|---|---|---|---|---|
| blue | **fold** — the sine folder | **absorb** — the loop's lowpass, 18 kHz→300 Hz | **cross** — how far the loop moves the source | **rate** — 0.02–40 Hz | **core** — triangle → dirty sine → saw → pulse |
| green | **time** — 20 ms–1.2 s | **warp** — sub → delay time | **polar** — below 50 ducks, above 50 lifts | **shape** — tri / ramp / square / S&H | **shape** — asymmetry, pulse width |
| white | **decay** — the loop's gain | **drift** — loop → pitch | **drive** — loop saturation, 1–3× | **depth** | **fuzz** — saturation |
| orange | **filter** — 60 Hz–9 kHz | **blend** — dry/wet | **tone** — the loop's highpass, 80 Hz–1.2 kHz | **dest** — filter / time / pitch / cross | **grit** — noise |

## Install

1. Download `sama.opmod` from the latest [Actions run](../../actions) or the
   [releases page](../../releases).
2. Back up the OP-1. Disk mode: `shift + COM`, then `T3`.
3. Copy `sama.opmod` into `mods/` — **not** `mods/bridge`.
4. Eject, restart.
5. `shift + T1` → engine list → **sama**.

Requires Opus 2 core **2.8** or newer.

## Building

CI builds the `.opmod` with the Blackfin toolchain (GNU binutils 2.38 + GCC 13.3, C only, no
libc) and uploads it as an artifact; tagged builds attach it to a release.

Locally:

```sh
git clone --depth 1 https://github.com/LizardMan245/opus /opt/opus
cd /opt/opus/plugin-kit
bash tools/toolchain/build-bfin-toolchain.sh fetch verify extract linux

export PATH=$HOME/bfin-build/linux-install/bin:$PATH
export OP1_BFIN_TOOLCHAIN=$HOME/bfin-build/linux-install
python3 /opt/opus/plugin-kit/plugin_tool.py build /path/to/this/repo
```

The toolchain is a Linux ELF binary; on macOS run it inside a Linux container.

## Testing

### Regression suite

`test/regression.py` builds the host harness — which compiles the **real** `sama.c` — renders
twenty parameter configurations, and asserts the instrument still behaves:

- it sounds, does not clip, and still sustains after the note stops;
- **all twenty controls change the waveform** (a dead control produces a bit-identical file);
- the loop decays when short and rings when long;
- the oscillator is in tune; the four cores differ.

```sh
OPUS_KIT=/path/to/plugin-kit/sdk python3 test/regression.py
```

This runs in CI on every push, before the plugin is built.

### Harness

`test/host.c` renders audio: the twenty arguments are the five faces in k99 (niche, space,
cross, mod, core), and it prints `peak / rms1 / rms2 / rms5 / zc`.

`test/render.c` runs the page functions and dumps a display list; `test/render.py` draws it
with the OP-1's own vector font — that is where the art above comes from.

## Repo layout

```
sama.c            the engine: voices, the room, the pages
sama_art.h        page point tables (generated by tools/gen_art.py)
sama_tab.h        DSP tables: delay lengths, filter, absorb, tone, breath (tools/gen_tab.py)
manifest.txt      the plugin manifest (id jake.sama, core 2.8)
cpu.txt           the declared CPU ceiling
tools/            table and art generators
test/host.c       host audio harness (real engine, stub API)
test/regression.py  the regression suite
test/render.c     host page renderer -> display list
test/render.py    display list -> PNG (OP-1 font)
art/              the screenshots in this README
```

## Credits

- [Opus 2](https://github.com/LizardMan245/opus) and its plugin kit.
- Mutable Instruments' open-source DSP, which informed the filter, the folder and the
  saturation approach.
- Make Noise's Strega, designed by Tony Rolando and Alessandro Cortini, for the architecture
  and the vocabulary.

## License

MIT — see [LICENSE](LICENSE).
