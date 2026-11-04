# Design

## Signal flow

```
keys ──► pitch[n] ──► [voice × 6]
                        triangle ─► saw ─► sine folder ─► × envelope ─► × swell ─► × cross
                                                                                     │
   sub (f/2) ──────────► warp   (delay time)                                         │
   loop envelope ──────► drift  (voice pitch)                                        │
   loop envelope ──────► filter (cutoff)                                             ▼
                                                                          sum, 64 samples @ 44.1 kHz
                                                                                     │
      ┌── feedback × decay ◄── drive ◄── absorb LP ◄── tone HP ◄── filter ◄──────────┤
      └──► delay line (20 ms – 1.2 s) ────► wet ────────────────────────────────────┴──► blend ──► out
```

The voices render at 88.2 kHz, 128 samples each; the loop runs once per block at 44.1 kHz on
the summed voices, which is Strega's topology (one delay after the mix) and the only affordable
one on a Blackfoot-class CPU.

## Why faces

Encoder turns reach a plugin engine **only on its own T1 page** on this firmware: a
plugin-owned `page_t2` / `page_t4` draws, but its encoders are never delivered — so an engine
that takes those pages gives you controls that cannot be turned.

sama therefore leaves **T2 and T4 to the system** (envelope, LFO) and puts everything on T1:
the four encoders are the OS's own, so they always work, and a **face** decides what they
mean. T3 and T4 page through five faces (niche, space, cross, mod, core) while the screen
stays on T1. Each face keeps its own four values: twenty parameters, four encoders, no turn
taken from the OS, and the system LFO still modulates the four T1 knobs.

## The DSP blocks

**Voice.** A selectable core (triangle, sine, saw, pulse) at the phase step given by the core (`pitch[n]`, which already
carries glide, bend and tuning), crossfaded to a saw, then a **sine wavefolder**: a 256-point
table read with linear interpolation, blended dry→folded so low drive bends and high drive
folds. Up to 8 folds. Integer only, output always inside ±32767.

**Delay.** 65,536 samples (1.5 s of headroom) at 44.1 kHz. Q8 fractional read, linear
interpolation, a write lowpass that closes as the delay lengthens (the PT2399 trick: longer
delay = narrower bandwidth), and faint clock noise at long times.

**The loop.** TPT state-variable lowpass with error-feedback carries (`e1`, `e2`) so the Q12
truncation does not stall the decay; a DC blocker at ~8 Hz; a highpass of its own (`tone`,
80 Hz – 1.2 kHz); `absorb` as the loop's lowpass (18 kHz – 300 Hz, per pass); soft-knee
saturation. `decay` sets the loop gain 0.458 – 1.002, so it owns the tail; `absorb` colours
each pass.

**Couplings.**
- `warp`: the note's sub-harmonic (f/2) modulates the delay time, per sample, added *after*
  the base time's slew so the slew doesn't remove it.
- `drift`: the loop's envelope (rectified, smoothed) bends the voices' pitch — the loop sags
  the note as it charges.
- `cross`: the loop's envelope moves the source level; `polar` chooses duck (−) or lift (+).
- `drive`: saturation in the loop, 1–2×, so the loop can bloom into self-oscillation.
- the loop's envelope also colours the filter (+4 cutoff steps).
- `breath`: a looping attack/decay function generator at 0.02–40 Hz, on the filter, the delay
  time, and (through `swell`) the voice level.

**Envelope.** The engine owns it (`OPUS_ENGINE_OWN_ENVELOPE`): attack, hold, release. A note
you let go keeps sounding for `hold` (0–27 s) and then releases — the drone is played, not
triggered.

## Numbers

- 6 voices × 128 samples at 88.2 kHz, one loop pass of 64 samples at 44.1 kHz.
- delay memory 131,072 bytes (`opus_alloc_audio`).
- voice state 20 bytes; the core's budget is 32.
- claimed CPU ceiling 200,000 cycles a block (a conservative figure; the device reports the
  real worst block in the mods list).

## What is deliberately not here

Tempo sync, sample-accurate delay jumps, a clean digital delay, a linear loop, a conventional
filter-with-envelope synth voice. The instrument's character is the loop: its loss, its
saturation, its own filter, and the couplings between the loop and the source.
