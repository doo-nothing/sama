# Release checklist

Short, enforceable, mirrors the CI gate. Do not publish or install on a device until
all seven are true.

1. Clean tree; `manifest.txt` version == tag == `build/sama.opmod` version.
2. `test/regression.py` green (including `SAMA_STATE` release-tail, load, scene, turn).
3. ASan/UBSan: state test, growl 99 at chords 36/44/48, the two room configs from
   `scratch-build/sama-audit/tests/`; no leaks in `test/host.c`.
4. Host + render compile with `-Wall -Wextra -Werror`; render emits 7 pages with the
   OP-1 font.
5. Kit pinned; build twice -> same sha; record the sha and the release asset sha.
6. Device: power-cycle, 7 faces, all 4 knobs each, saved sound restores all 28, new
   sound defaults, T4 LFO + release + face switch does not change the stored value,
   freeze/tune/scene, still no ghost.
7. `mods/sama.opmod` sha == the built sha; no `._*`; eject; 1.1.8 backup preserved.
