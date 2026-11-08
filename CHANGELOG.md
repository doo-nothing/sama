# Changelog

## 1.0.4

- **fixed: the six-voice gain table was out of bounds.** It was a 6-entry table indexed by
  the voice *count*, so six voices read element 6 (UBSan/ASan: stack-buffer-overflow; the
  garbage value was ~1.0e9, hard-clipped by the soft clip into broadband noise). The table
  now has a valid entry for every count 0–6 and is clamped. This is the main device-only
  white-noise cause.
- **fixed: the room feedback stored the unclamped filter output in a short.** At high
  resonance the filter can exceed ±32767 (53% of samples at res=99), so the room read back a
  wrapped value. It is clamped before the store.
- **fixed: the growl multiply could overflow signed 32-bit** at high notes (chord 44,
  growl 99); it uses 64-bit now.
- **fixed: signed left shifts** in the pitch quantizer and the SVF error terms (UB), and the
  tuned highpass multiply now uses 64-bit.
- **fixed: the host harness wrote 64 samples past the render buffer** on the last block
  (test harness only).
- **tests:** six-voice flatness/harmonic checks (the old test only checked peak/clip, and
  the soft clip caps at 21,843 so the clip count can never fail); a high-note growl test; a
  high-resonance room test; a CI sanitizer run (ASan/UBSan, six voices). The page says
  **v1.4**.

## 1.0.3

- **fixed: plain T1 on the T1 page was hijacked as "next scene".** T1 is the page key, so
  pressing it to return to T1 cycled scenes — and could land on **storm**, the deliberately
  self-oscillating room, which sounds like a loud unstable drone. Plain T1 now passes to the
  OS; **FN + T1** loads the next scene; **shift + T1** freezes. The manifest declares only
  `t1 fn` and `t1 shift`. The page says **v1.3**.

## 1.0.2

- **fixed: the sound-data load was in `v_init`, which can run in the audio interrupt.** It is
  now deferred to `opus_on_block` (the sequencer thread): `v_init` only raises a flag, and the
  data is read a block later. This removes the last known audio-unsafe call from the engine.
  The first block after a sound change uses the default faces; the saved ones arrive one block
  later. The page says **v1.2**.
- the persistence test drives the deferred load explicitly (`e_on_block`) and still restores
  all six faces.

## 1.0.1

- **fixed: the room→voice feedback buffer was read by the audio interrupt while the block
  rewrote it.** It is now double-buffered (the block writes one 64-sample buffer and flips;
  the voices read the last complete one). This was the device-only white-noise/roughness
  source; the host's sequential render never hit it.
- **fixed: T4 was declared and taken by the engine**, stealing the system's LFO page. T3 pages
  the faces on T1; T4 is the system's page again on T1 and T2. The four encoder pushes still
  jump to niche / space / cross / core, and T1 still loads scenes / shift+T1 freezes.
- **fixed: 32-bit overflow paths** in the folder, fuzz, gate, output and the soft-clip table.
  The folder/fuzz/gate/output products use wide arithmetic, the voice output is clamped before
  the gate, and the soft clip has an unsigned-magnitude guard. High fold/fuzz can no longer
  send a negative magnitude into the table read.
- the page says **v1.1**; version 1.0.1. The regression key test now expects T4 to pass and T3
  to page.

## 1.0.0

**sama 1.0: one room, tuned and coupled.** The instrument now has a body, a performance layer,
and the tests to prove it still behaves.

- **six faces**: niche, space, cross, mod, core and a new **macro** face (body / pressure /
  motion / space). The four encoder pushes jump to the first four; T3/T4 page the rest.
- **tuned room** (`tune` setting): the time knob selects 1–16 periods of the lowest held note,
  the loop runs clean, and the filter/resonance/tone move to the room's output. High decay now
  self-oscillates *in tune* with the note instead of at a random resonance.
- **room → voice feedback**: the room's last 64 samples are buffered and phase-modulate the
  oscillator at audio rate (depth from `cross`, sign from `polar`), with a hard cap and a
  highpass. One block of latency.
- **playability**: a note-on strike (folder + click + filter), root-relative filter/folder key
  tracking, four scenes (lamp / choir / comb / storm) on T1, and a freeze on shift+T1.
- **timbre**: filter resonance Q 0.5–8; continuous core morph (tri/sine/saw/pulse) with polyBLEP;
  `bias` and `growl` replace the old shape/grit (folder symmetry, sub→phase FM); a 223-sample
  allpass diffuser; tape wow; a quiet f/2 sub; a cubic soft clip with **unity small-signal gain**.
- **pitch**: a semitone ratio table; slow pitch mod quantised to semitones or the chosen scale
  (chromatic/major/minor/pentatonic); fast pitch mod interpolates the table for continuous FM.
- **gain staging**: one voice has headroom, a 1/√n voice-count gain, and one final soft clip. A
  triangle keeps its third harmonic; six voices stay bounded.
- **envelope**: the room envelope is mean-abs/peak over the block, not one sample; the cross gate
  is ramped per sample; the shared warp follows the lowest active voice at f/2, not the last
  voice at f/8.
- **persistence**: all 24 face values plus the face travel in `opus_engine_data`; a saved sound
  restores the whole instrument. The OS's four knobs for the saved face are merged over the data.
- **control map**: T4 on T2 goes to the system; T4 on T1 pages a face; the manifest, README and
  code agree. T2/T4 remain the system's envelope/LFO pages.
- **tests**: `test/analyze.py` (f0, centroid, harmonic ratio, crest, pitch/AM tracks, bands);
  `test/regression.py` now covers six voices, the gain/3rd harmonic, pitch/cross depth, the f/2
  warp, the tuned room, the scale, freeze, scenes, persistence, key routing, every deep control
  and macro, and an extreme six-voice setting. CI installs numpy.
- **build**: `tools/build-opmod.sh` builds the `.opmod` in a cached Linux container on macOS;
  version 1.0.0; CPU claim 320,000 cycles (device measurement pending).

Fixes along the way: the loop's soft clip lost gain as the knee narrowed (the tail shortened with
drive); the tuned room's first highpass-bypass attempt silenced it; the freeze could not overcome
the loop's filter loss; the dead `g_breath`/`tlen` code, the stale "v18", the generator writing
outside the repo, and the stale face defaults/comments are gone.

## 0.7.1

- **fixed: drive added loop gain.** It multiplied the feedback path, so the loop
  self-oscillated at full scale and pinned the output — every knob felt weak because the
  instrument was saturated. Drive is now a movable soft knee: it colours the loop without
  changing its gain.
- fixed: the sample-and-hold modulator never held (it only sampled in a 3000-wide window);
  fixed: the cross destination's modulation stuck between blocks; fixed: the whirl orbit never
  moved; fixed: a sign-compare warning that broke the device build.
- absorb now runs 12 kHz→150 Hz on a squared curve, so a turn near the top is audible.
- warp depth raised; grit raised; the mod page defaults deeper and faster.
- regression suite extended: tail-after-release, decay shapes the tail, and a sensitivity pass.
- the loop's resonance was lowered (Q≈0.8) so it rings rather than self-oscillates.

## 0.7.0

- **T2 and T4 are the system's pages again.** Our pages live on T1, paged with T3/T4; the
  system LFO keeps modulating the T1 knobs.
- five faces: niche, space, cross, mod, **core**.
- **core page**: the oscillator selectable between triangle, dirty sine, saw and pulse, with
  shape, fuzz and grit.
- **mod page**: an internal modulator with rate, shape (tri/ramp/square/S&H), depth and a
  destination (filter, time, pitch, cross).
- **blend** replaces the fixed dry/wet.
- the private envelope is gone: the OS owns the amplitude, so the system envelope page works;
  the drone comes from the loop.
- **regression suite** (`test/regression.py`) runs in CI: twenty controls, the loop's decay
  and sustain, tuning, and the four cores.
- fixed: fold was dead (k99 fed into a Q15 formula), shape did nothing outside the pulse core,
  grit was a constant offset the DC blocker removed.

## 0.6.0

- fourth face **cross**: `cross` (depth), `polar` (duck/lift), `drive` (loop saturation),
  `tone` (loop highpass).
- the whirl page (T4) is back; T2 and T4 now select their face.
- kindle's fourth knob is `swell` (breath → level), since cross moved to its own face.
- output trim and a wider level range.

## 0.5.1

- DSP pass from the Mutable Instruments research: SVF error feedback (the truncation tail
  dropped from −27 dBFS to silence), `a3` guard, interpolated folder with a dry→fold blend,
  tighter state clamps.
- folder replaced by a sine folder (no 32-bit overflow, no reflection loop).

## 0.5.0

- absorb became the loop's lowpass (18 kHz – 300 Hz) instead of a second gain; decay owns the
  tail (0.458 – 1.002).
- time slew split: base slewed, modulation added after it; warp is now per-sample.
- the loop's envelope is rectified (it was averaging to zero, which killed `cross` and
  `drift`); it ducks the source, colours the filter, and sags the pitch.
- defaults tuned from the control-matrix study.

## 0.4.1

- the loop can sustain: decay and absorb spans widened.
- the sub-oscillator advances at the right rate (it was running at f instead of f/2).
- warp depth corrected; breath tremolo raised from 1.1 dB to audible.
- encoder pushes no longer flip the face.

## 0.4.0

- engine reads the sound's knobs instead of the voice block (the bug that zeroed every face
  on load and collapsed the delay).
- sine wavefolder, loop highpass, sub → pitch coupling.

## 0.3.0

- faces over the four T1 encoders; full-page redraw (no still cache, no ghosting).
- T2 and T4 handed to the OS pages.

## 0.1.0

- first working build: oscillator, shared delay loop, the kindle envelope, the whirl.

## 0.0.1

- initial scaffold.
