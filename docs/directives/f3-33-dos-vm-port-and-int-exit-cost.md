# f3-33 — DOS VM monitor cost on the T23: PIT/PIC port exits and real-mode INT reflection

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-33` from main `bbac0713`.
- **Authority:** `src/kernel/vm/dosvm_bios.c` (port dispatch), `src/kernel/vm/dosvm_io_burst.inc`, `src/kernel/vm/v86_emulate*.c` (INT/IRET reflection), `src/kernel/vm/dosvm_pit*.c`, host tests, `docs/design/dos-dpmi-contract.md` and `docs/validation/2026-10-15-f3-31-performance.md` (amend).

## Evidence (T23, image e8dfd0cd, `legacy/local/physical/e8dfd0cd/serial.log`, `vm_perf_*` records of the Quake and DOOM sessions)

Per five-second interval: the VM task takes 84–87 % of wall time but guest code only 21–38 %; the monitor takes 2.3–3.3 s. Top causes (`vm_perf_cost`, overlapping, not additive):

- `db` 2.0–2.2 s per interval, 420–455 k exits (90 k/s, 4.9 us each): the DPMI return tracker's single steps (f3-10). This is the largest cost and is **out of scope here** (contract review pending with the owner).
- `pit_pic` 0.4–0.9 s per interval, 53–115 k exits (8 us each): IN/OUT to 0x40–0x43 and 0x20/0x21 (Quake reads the PIT counter for its clock; DOOM's timer ISR acknowledges the PIC).
- `rm_int` 1.0–1.5 s per interval during loading, 43–69 k real-mode INT reflections (16–25 us each) and `rm_sti` 0.2–0.3 s; `pm_flags` 0.5–0.6 s (7 k exits, 80 us each).
- `vga_io` up to 1.0 s during DOOM (86 k exits, 12 us each) despite the batched OUT path.

## Required

1. Measure each exit's fixed overhead on the host model (trap entry, frame save, decode, dispatch, resume) and reduce it: a direct dispatch for the hot ports (PIT counter latch/read, PIC OCW2 EOI and IMR reads) without the generic decoder; the PIT read sequence (OUT 0x43 latch, IN 0x40, IN 0x40) handled as one exit where the guest's instruction stream allows it (same burst mechanism as the VGA OUT batching, f3-19).
2. `pm_flags` at 80 us per exit: find what makes a protected-mode flag exit ten times costlier than a port exit and fix it.
3. `rm_int` at 16–25 us: profile the reflection path (vector read, stack push, frame rewrite) and the software-interrupt return; remove per-exit work that does not depend on the vector.
4. Do not grant physical ports, do not change the DPMI return tracker, do not change guest-visible semantics; host differential tests against the current behaviour, the Quake and DOOM host replays (`tests/host/dosvm_guest`) with before/after per-exit costs; `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`, `python3 scripts/build_dos_image.py`. Report in `build/codex/f3-33-report.md`. The lead runs f3-vm, dos4gw and quake on QEMU and compares the T23 `vm_perf_cost` rows.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
