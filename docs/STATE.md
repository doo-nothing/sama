# State machines

sama is small, but ten pieces of state drive everything the player sees and hears. Most
of the 1.1 regressions were not DSP bugs — they were two of these machines disagreeing
about who owns a value. This document makes them explicit: the states, the events, the
invariants, and the consequence when an invariant breaks.

The implementation is still the single `sama.c`, so this is a specification plus the
small explicit transitions added in 1.1.6. The host harness (`test/host.c`,
`SAMA_STATE=1`) runs the same transitions the device runs and the regression asserts
the invariants below.

## 1. Sound load

| | |
|---|---|
| state | `g_load_state` (`LOAD_IDLE`, `LOAD_PENDING`), `face`, `g_scene`, `fv`, `fvl` |
| events | `v_init(index 0)`, `e_on_block` (sequencer thread), `save_data` |
| owns | the saved 68 bytes and the first copy into both face arrays |
| invariant | after `e_on_block`: `face` in range, `g_scene` in range, `fvl[f][i] == fv[f][i]` for all 28 |
| failure | 1.1.4 loaded `fv` but not `fvl`; every saved sound played factory defaults on its non-active faces |
| transition | `v_init` → `LOAD_PENDING`; `e_on_block` consumes it exactly once, then syncs both arrays |

Empty/invalid data is its own terminal state: `face = niche`, `g_scene = 1`, the factory
choir table into `fv`, and the same table into `fvl`.

## 2. Data format

| | |
|---|---|
| state | `struct sama_data` (68 bytes), v1 (60 bytes) |
| fields | magic, version, face, 28 × 0..99, scene |
| invariant | magic/version/face/scene checked; every value `cl99`; the struct is zeroed before read and write |
| failure | uninitialized tail bytes travelled with a saved sound; v1 migration was untested |
| transition | v2 → load; v1 → load + neutral tune (`50,0,0,99`) + scene 1; anything else → empty |

`scene` reuses the two former tail-padding bytes. The size stays 68, so 1.1.0–1.1.5
sounds still load and old readers simply see scene 0. No version bump, no broken data.

## 3. Face

| | |
|---|---|
| state | `face` (0..6), `lock_blocks` |
| events | T3, the four pushes, `load_scene`, `set_face` |
| invariant | lock set **before** any mutation; `fv`/`fvl` of the new face agree; the OS knobs are written to the new face; save happens after those writes |
| failure | the old `set_face` set `lock_blocks` after `face`/`save_data`, so an audio block during the save could copy the old OS knobs into the new face |
| transition | `lock=12` → `face` → `fvl sync` → `knob_set` → `save` → `lock=6` |

## 4. Knobs (stored / live / OS)

This is the machine that produced the dead-knob 1.1.0 and the baked-LFO 1.1.2.

| | |
|---|---|
| state | `fv` (stored user value), `fvl` (live, LFO included), the OS's four knobs |
| events | `e_turn` (user), `b_block` (each audio block), note on/off |
| invariant | `fv` is only written by a user turn or when no voice is active; `fvl` may be written by the LFO; `save_data`/`set_face`/`load_scene` read `fv`; the DSP and page read `fvl` |
| failure | 1.1.0 wrote neither (dead knobs); 1.1.2 wrote the LFO into `fv` (saved the wobble) |
| transition | turn → both; voice active → `fvl` only; no voice → both (the OS's stored value) |

The 1.1.2 lesson: `opus_engine_knobs` is LFO-live while a note sounds. There is no getter
for the unmodulated stored value, so the plugin owns `fv`. That is why the per-block copy
must not be the only control path, and why `e_turn` still exists.

## 5. Scene

| | |
|---|---|
| state | `g_scene` (0..3) |
| events | FN+T1, `load_scene`, load, new sound |
| invariant | every scene writes all 28 values and both arrays; the scene is saved and restored; the dots show the loaded scene |
| failure | `g_scene` was never persisted, so the dots lied after reload and FN+T1 restarted from 1 |
| transition | `lock=12` → `g_scene` → all `fv` → all `fvl` → `knob_set` → `save` → `lock=6` |

## 6. Tune mode

| | |
|---|---|
| state | `g_tune` (setting), `g_tuned_k`, `g_master` |
| events | FN+T3, setting load |
| invariant | tune mode locks the room root and the voices to the same master ratio; `1/k` is shown; turning tune off clears the readout |
| failure | none found in 1.1.4; the earlier −62 cent default came from drift, not this machine |

## 7. Freeze

| | |
|---|---|
| state | `g_freeze`, the tone-highpass state `hlp` |
| events | shift+T1, load |
| invariant | frozen = still room with **no DC** (`hlp=0`), opened filters, page/message |
| failure | 1.1.4 froze the highpass state nonzero, leaving a constant −0.0064 offset |
| transition | freeze on → zero `hlp`/`e1`/`e2`; freeze off → normal path |

## 8. Page / still cache

| | |
|---|---|
| state | `pg_still_key(face)`, the text-slot pins, `g_n` |
| events | face change, scene, freeze, tune, message |
| invariant | the static block depends only on `face`; live values, the scene dots, the lamps, the message and the horizon are drawn outside it; a key hit draws zero static ops |
| failure | the key included freeze/scene (wasted redraws) and the page captured `face` then re-read it (a face change could cache the old art under the new key) |
| transition | key = `face`; scene dots moved live; `f` captured once |

Text inside the still is legal per the API but is the one thing to confirm on hardware;
if it ghosts, fall back to drawing the chrome live.

## 9. Settings

| | |
|---|---|
| state | `g_scale`, `g_tune`, `g_wow`, `g_keytrack`, `g_diffuse` |
| events | FN+T3, the OS's setting rows, boot |
| invariant | each setting reads the same value at boot and at runtime; `e_setting` and `init` agree |
| failure | none known |

## 10. Release

| | |
|---|---|
| state | source commit, tag, manifest version, `.opmod` sha, installed sha, backup set |
| events | commit, tag, CI, install, device check |
| invariant | version == tag == manifest; tests green; the installed sha matches the built sha; a backup of the previous device build exists; `._*` removed |
| failure | 1.1.0/1.1.1/1.1.2/1.1.3/1.1.4 were built from dirty trees and no control test gated the install |

This is the machine that let the others ship. The gate is the checklist in
`docs/CHECKLIST.md` (kept short): clean tree, version/tag, tests, sha, device smoke.

## Why this shape (UX / DX / AX)

- **UX:** one stored value per knob, one live value, one scene. Turning a knob always
  moves the number the player set; the LFO can wobble the sound without rewriting the
  patch; reloading shows the patch that was saved.
- **DX:** every transition has a name, a lock rule, and a regression check. The host can
  run the same transitions without audio; a failure names the machine, not "it sounds
  wrong".
- **AX:** the state is inspectable (`SAMA_STATE=1` prints the transition results), the
  doc is the contract, and the test matrix is the gate. An agent can diff the invariants
  instead of guessing from a WAV.

## Test matrix

| invariant | host | regression | device |
|---|---|---|---|
| init face/scene/sync | `SAMA_STATE` | `state: init …` | page + four knobs |
| face switch lock + sync | `SAMA_STATE` | `state: face …` | T3 across all faces |
| turn writes both | `SAMA_STATE` | `state: turn …` | each encoder |
| scene writes all 28 | `SAMA_STATE` | `state: scene …` | FN+T1 ×4 |
| save/reload face/scene/values | `SAMA_STATE` | `state: reload …` | power-cycle, load |
| new sound defaults | `SAMA_STATE` | `state: new …` | new Sama sound |
| LFO does not bake | stub (future) | — | T4 LFO held, switch face/back |
| freeze has no DC | render | existing | freeze tail |
| still key face-only | render stub | future | T3 cycle |
| version == tag == sha | — | CI | `show` on device |

The LFO-bake and still-hit rows are the two remaining host gaps; both are listed in
`TODO` in the repo. They do not block the 1.1.6 install because the device check covers
them, but they are the next tests to write.
