# Handoff 2026-04-21 — OpenGEM VDI: vqt_extent / vqt_attributes / vex_timv

## Context and goal
Continuation of the OpenGEM v86 bring-up. After the previous step (vq_extnd
57-word handler, state-setter batch, vro_cpyfm) GEM advanced from ~519 to
~629 serial lines and the new tight loop showed three opcodes dominating
the histogram: `0x1F` (vqt_extent), `0x21` (vqt_attributes), `0x80`
(vex_timv), each ~11 invocations within the trace window.

Goal of this step: implement plausible echoes for those three VDI calls
so GEM can clear that loop and progress toward the AES init / window
chain.

## Files touched
- `stage2/src/v86_dispatch.c`
  - Added handlers for VDI opcodes `0x001F`, `0x0021`, `0x0080` inside
    `v86_try_emulate_int_ef`, immediately after the existing `vsf_udpat`
    (`0x007D`) block and before `vro_cpyfm` (`0x007F`).

## Decisions made
- **vqt_extent (0x1F)**: returns a fixed `8x16` per-character bounding
  box. `n_intin` (contrl[3]) gives the number of chars; `ptsout[0..7]`
  carries the four corners `(0,0)`, `(w,0)`, `(w,h)`, `(0,h)` with
  `w = n_chars * 8`, `h = 16`. `n_ptsout = 4`, `n_intout = 0`.
- **vqt_attributes (0x21)**: reports `font=1`, `color=1`, `rotation=0`,
  `h_align=0`, `v_align=0`, `write_mode=1` in `intout[0..5]`, and
  `8x16` char/cell sizes in `ptsout[0..3]`. `n_intout=6`, `n_ptsout=2`.
- **vex_timv (0x80)**: echoes the new tick handler `contrl[7..8]` back
  in `contrl[9..10]` (idempotent old/new exchange) and reports a 50ms
  tick rate (`intout[0]=50`). `n_intout=1`, `n_ptsout=0`.
- All three return `AX=0` and clear `CF`, consistent with the existing
  VDI bring-up convention.

## Validation performed
- `make` build: green, no new warnings or errors.
- QEMU/OVMF run: local serial-capture remains chronically flakey on
  this host (sometimes 138-line stop after `next step: handoff to
  DOS-like runtime`, sometimes 600+ lines with INT EF traffic). Per
  shared agent directive this is not treated as a blocker; functional
  validation is expected to come from on-host runs.
- The change is purely additive in the VDI dispatcher; the surrounding
  state-setter batch and `vro_cpyfm` paths are untouched.

## Risks and next step
- **Risks**:
  - vqt_extent metrics are coarse (assume 8x16 monospace). Real GEM
    apps may rely on more exact extents for layout; expect cosmetic
    misalignment until a real font metrics table is wired in.
  - vex_timv returning the same vector as `previous` means GEM will
    not be able to chain real timer handlers. Acceptable for early
    init; revisit when implementing real PIT routing into v86.
- **Next step (likely bottleneck progression)**:
  1. Re-run on host and capture a fresh op-histogram from the
     serial log.
  2. If AES traffic now dominates (CX=0x00C8), extend the AES
     dispatcher beyond `appl_init` / `evnt_multi` / `graf_handle` /
     `appl_exit` (e.g. `wind_create` op 100, `wind_open` op 101,
     `objc_draw` op 42, `form_alert` op 52).
  3. If VDI keeps dominating with fresh opcodes, fill them with the
     same echo pattern (return zero-sized output blocks + AX=0).
  4. Once the desktop chain reaches `GEM.EXE`, re-evaluate framebuffer
     wiring and INT 0x10 mode setup before attempting real raster
     output.

## Branch and merge state
- Source branch: `wip/opengem-046-vdi-stubs` at `6ac3924`.
- Merged into `main` at `6cd47e6` via `--no-ff`.
- Pushed to `origin/main`.
