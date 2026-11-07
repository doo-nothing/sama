# Design

## Signal flow

```
keys ──► pitch[n] ──► [voice × 6] @ 88.2 kHz
                        core morph ─► bias ─► fuzz ─► sine folder ─► + sub
                          ▲  ▲                                  │
                          │  └── room audio (phase, 1 block)     │
                          │                                     ▼
   sub (f/2) ─────────────┴──► growl (phase FM)          × strike, × cross gate
   room envelope ─────────────► drift (voice pitch)              │
   root ──────────────────────► key track (filter/folder)       ▼
                                                  sum, decimate, 64 samples @ 44.1 kHz
                                                                 │
      ┌── decay ◄── soft clip ◄── write LP ◄── [tone HP] ◄── filter ◄── diffuser ◄──┤
      │                                                          (tune: clean)     │
      └──► delay line (20 ms–1.2 s; tune: 0.7 ms–1.2 s) ─────────► room ────────────┴──► blend ─► out
                                                                     │
                                                                     └── 64-sample room buffer ─► voices
```

The voices render at 88.2 kHz, 128 samples each; the room runs once per block at 44.1 kHz on the
summed voices. That is Strega's topology (one delay after the mix) and the only affordable one on
a Blackfin-class CPU.

## The two room paths

**Free mode (default).** The delay read goes through a short allpass diffuser, a TPT state-variable
lowpass (60 Hz–9 kHz, Q 0.5–8), a DC blocker, a tone highpass and a write lowpass that closes as
the delay lengthens. The filter and the tone are *inside* the loop; the output is the dry voice
blended with the room. This is the aging, dirty room: the comb is inharmonic because the filters
add frequency-dependent delay, which is part of the character.

**Tune mode.** The loop is the delay read and the gain only; the filter, resonance, tone and
diffusion move to the room's *output*. The time knob selects `k = 1..16` periods of the lowest
held note (computed from its pitch step, not a MIDI note), so the comb is in tune with the root.
The write lowpass is opened. High decay makes the room self-oscillate at the note; the output
filter shapes the harmonics without detuning the loop.

## The voice

- **Core.** A continuous morph across triangle → sine → saw → pulse. Saw and pulse use polyBLEP
  corrections at their discontinuities; the morph crossfades adjacent shapes.
- **Bias.** A DC/asymmetry offset before the folder; on the pulse core it also sets the width. The
  voice's DC blocker removes the offset after it has bent the folder.
- **Fuzz.** A cubic soft clip with a drive of 1–3.3×.
- **Folder.** A 256-point sine folder, 1–8 folds, dry→folded crossfade. The fold amount is opened
  by the note-on strike and reduced slightly for high notes.
- **Sub.** A quiet triangle at f/2 (about −22 dB) for weight; `growl` phase-modulates the main
  oscillator with that sub (0–19 %), which is the vocal/reedy edge.
- **Strike.** A 2–30 ms per-voice transient on note-on: it opens the folder, adds a short noise
  click and opens the room filter. The OS's amplitude envelope is separate and slow.
- **Room audio.** The last block's room signal is interpolated to 88.2 kHz and added to the
  oscillator phase (depth from `cross`, sign from `polar`), and a quarter of that depth bends the
  folder. One block of latency, a DC blocker and a hard depth cap.
- **Drift.** The room's mean-abs envelope bends the voice pitch; `polar < 50` sags, `> 50` lifts.
- **Key track.** The root's distance from A440 offsets the loop filter (0–100 %), and the voice's
  own pitch offsets its fold amount.

## The room

- **Delay.** 65,536 samples, Q8 fractional read with linear interpolation, 20 ms–1.2 s. The time
  slews at ~7 Hz; `warp` (the root's sub) adds a per-sample delay-time modulation at f/2; `wow`
  adds a slow tape wobble. In tune mode the warp, wow and time LFO are disabled.
- **Diffuser.** A 223-sample Schroeder allpass (about 5 ms), coefficient 0–0.6, in the loop in
  free mode and on the output in tune mode.
- **Filter.** TPT SVF lowpass with error-feedback carries (`e1`, `e2`), Q 0.5–8, 60 Hz–9 kHz,
  key-tracked; the resonance is not gain-compensated, so high Q + high decay can self-oscillate
  (the soft clip bounds it).
- **Tone.** A one-pole highpass 80 Hz–1.2 kHz and a write-lowpass tilt; `tone` moves both.
- **Drive.** A 129-point `u − u³/3` soft clip with a knee of 6 k–27 k. The table has unity
  small-signal gain, so drive colours the loop without changing its gain. (The earlier
  piecewise-linear clip lost gain as the knee narrowed, which quietly shortened the tail.)
- **Decay.** Loop gain 0.458–1.032; the top of the range can self-oscillate. The soft clip bounds
  the level.
- **Couplings.** The room envelope ducks/lifts the source (`cross`, `polar`), the room audio
  bends the voice phase (audio rate), the root drives the warped delay time and the filter key
  track, and the filter/resonance/decay set how the room ages.

## The modulator

A looping shape (triangle, rising ramp, square, sample & hold) at 0.02–40 Hz, with depth and one
destination:

- **filter** — the loop cutoff (also gets a touch of always-on breath);
- **time** — the delay time (disabled in tune mode);
- **pitch** — slow rates are quantised to semitones or the chosen scale; fast rates interpolate
  the semitone table for continuous FM;
- **cross** — the gate/feedback coupling.

The LFO itself is resolved once per block; the pitch ratio is applied per voice. The filter/time
destinations are block-rate by design.

## The performance layer

- **Macro face.** Four offsets on the deep values, neutral at 50: `body` (filter/decay/tone/res),
  `pressure` (fold/drive/fuzz/bias/growl), `motion` (depth/rate/warp/drift/wow), `space`
  (blend/diffusion/tone/cross). The macros are always active; the deep faces are the base.
- **Scenes.** Four complete 24-value starting points: lamp, choir, comb, storm. `T1` loads the
  next; the current values can be saved as a normal sound.
- **Freeze.** `shift + T1` mutes the input, opens the tone/write filters, sets the loop gain just
  below unity and lets the room drone; the four voices are no longer heard, the room is.
- **Persistence.** The engine stores `{magic, version, face, 24 values}` in `opus_engine_data` with
  the sound. On load it restores all six faces; the OS's four knobs for the saved face are merged
  over the data so the current face is never stale.

## Why faces

Encoder turns reach a plugin engine **only on its own T1 page** on this firmware: a plugin-owned
`page_t2` / `page_t4` draws, but its encoders are never delivered — so an engine that takes those
pages gives you controls that cannot be turned.

sama therefore leaves **T2 and T4 to the system** (envelope, LFO) and puts everything on T1: the
four encoders are the OS's own, so they always work, and a **face** decides what they mean. T3 and
T4 page through six faces while the screen stays on T1. The four encoder pushes jump directly to
niche / space / cross / core. Each face keeps its own four values, and the whole set travels with
the sound.

## Numbers

- 6 voices × 128 samples at 88.2 kHz, one room pass of 64 samples at 44.1 kHz.
- delay memory 131,072 bytes; diffuser 446 bytes; voice state 20 bytes (core budget 32).
- The engine's data block is 60 bytes (magic, version, face, 24 values).
- Claimed CPU ceiling 320,000 cycles a block (conservative; device measurement pending — the
  original 0.7.1 claim was 200,000).
- Five settings: scale, tune, wow, keytrack, diffuse; plus the engine's own data.

## Tests

`test/host.c` compiles the real engine, renders one or six voices, and exposes the settings,
scenes, freeze, persistence and key routing through environment variables. `test/analyze.py`
measures f0, centroid, harmonic ratio, crest, pitch span, AM depth and more. `test/regression.py`
checks the gain, the six-voice bound, the pitch/cross destinations, the f/2 warp, the tuned room,
the scale, freeze, scenes, persistence, key routing, every deep control and macro, and an extreme
six-voice setting.

## What is deliberately not here

Tempo sync, sample playback, an audio input, a clean digital delay, a linear loop, a conventional
filter-with-envelope voice, a sixth deep face. The instrument's character is the room: its loss,
its saturation, its own filter, and the couplings between the room and the source.

The room feedback has one block of latency; the filter/time modulation is block-rate. The voice
pitch and the audio-rate feedback are the fast path.
