# f3-38 — windowed DOS games: VGA port exits and the planar presenter cost

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-38` from main `e7bdd513`.
- **Authority:** `src/kernel/vm/dosvm_bios.c` (port dispatch), `src/kernel/vm/dosvm_io_burst.inc`, `src/kernel/vm/dosvm_vga.c`, `src/kernel/vm/virtual_vga.c`, `src/kernel/vm/vga_presenter.c`, host tests, `docs/validation/2026-10-15-f3-31-performance.md` (amend). Not yours: the return tracker, PIT/PIC paths (f3-33), the native full-screen provider.

## Evidence (T23, image a79f840c, `legacy/local/physical/a79f840c/BOOT.LOG`, windowed DOOM interval n=39; owner 2026-10-17: "DOOM in finestra lagga tanto", Wolf3D likewise, Quake "discreto")

```
vm_perf_cost  vga_io=1440 ms / 103,432 exits (14 us each)   db=118 ms   pit_pic=61 ms
vm_perf_video ms=5000 frames=16 flips=0 harvests=15 pages=420
vm_perf_present converted=1,792,000 rows=5,730 present_us=118,343
```

Sixteen presented frames in five seconds (about 3 fps) while 29 % of the machine goes to VGA port exits: DOOM's Mode Y renders by column with a sequencer map-mask write per plane change (and Wolf3D similarly); the f3-19 batched OUT path covers adjacent sequences but each exit still costs 14 us.

## Required

1. Measure on the host model the per-exit fixed cost of a VGA sequencer/graphics-controller port write (trap, frame, decode, dispatch, PTE batch) and the fraction that is the batched PTE exchange of the planar aperture; then: direct dispatch for 3C4/3C5 and 3CE/3CF byte and word OUTs (as f3-33 did for PIT/PIC), the common map-mask sequence `MOV DX,3C4h / MOV AL,02h / OUT DX,AL / MOV AL,mask / INC DX / OUT DX,AL` (and the word form `OUT DX,AX`) retired as one exit, and a plane-change path that does not touch page tables when the previous and new plane selections map the same private aperture page set (keep the guest-visible latch/plane semantics exact against QEMU vga.c).
2. Presenter: the planar (Mode X/Y) row conversion and dirty harvest cost per frame on the host model; present only the changed rows (the record shows 5,730 rows converted for 16 frames); keep the exact scanout semantics (pan, split, wrap).
3. Target on the host DOOM replay (`tests/host/dosvm_guest`, `GUEST_PERF=1`): at least a 2x reduction of `vga_io` cost per frame and the presenter cost per presented frame, with before/after numbers; differential tests against the scalar model. Run the VM host suites, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`, `python3 scripts/build_dos_image.py`. Report in `build/codex/f3-38-report.md`. The lead runs doom/dos4gw/quake on QEMU and the owner the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No physical port grant.
