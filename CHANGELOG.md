# Changelog

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
