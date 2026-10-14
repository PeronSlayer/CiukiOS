# f3-16 — DOOM at 10 fps on the T23: planar VGA (Mode Y) must run through the direct window without faults

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f3-16` from `main`. Host-testable (`tests/host/dosvm_video_test.inc`, `dosvm_present_bench.c`, `dosvm_speed_test.inc`); the lead measures on QEMU and the T23.

## Evidence (T23, image fb3147dc, DOOM 1.9 shareware, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`)

`vm_beat_vga frames=226 flips=103 linear=0 planes=7031 faults=23812 direct=1 chain=0` after about 20 s of DOOM before the f3-15 freeze: ≈10 frames per second, **linear=0** (the f3-14 mode 13h fast path never engaged), **7031 plane switches** and **23812 page faults** on the A000 window. DOOM 1.9 renders in "Mode Y" (320x200 unchained, four planes, page flipping through the CRTC start address 0C/0D) and writes each plane with the sequencer map mask (3C4 index 2 = 1, 2, 4, 8) in `I_FinishUpdate`; `virtual_vga.c:375-399`: `cvga_direct_linear` requires map mask 15 and chain-4, `cvga_direct_plane` admits one plane at a time; `dosvm_vga.c:136-242`: each mask change re-maps the window and `video_harvest` copies the plane; stores outside the mapped plane fault. Wolf3D (Mode X-like, planar, latch copies) has the same cost model. Owner targets: DOOM ≥ 35 fps (its tic rate), Wolf3D ≥ 70 fps, on the 1.13 GHz Pentium III-M.

## References

FreeVGA (sequencer map mask, write modes 0-3, latches, unchained 256-colour modes), Michael Abrash's Graphics Programming Black Book ch. 47-49 (Mode X, Mode Y), DOOM source `i_ibm.c` `I_FinishUpdate`/`I_UpdateBox` (planar copies, page flip via CRTC 0C/0D, `outp(SC_INDEX+1, mask)` per plane), Wolfenstein 3-D source `ID_VL.ASM`/`ID_VL.C` (write mode 1 latch copies, `VL_Plot`, map mask usage), Intel SDM Vol. 3A §4.8 (accessed/dirty bits per 4 KiB page, already used for the direct pages), DOSBox `vga_memory.cpp` (planar write handlers, dirty tracking).

## Task

1. **Zero-fault planar steady state.** Keep the A000 window mapped read/write to a shadow while the guest writes one plane; on a map-mask change (port 3C5, already trapped), on a CRTC start-address flip and on present, harvest **only the pages dirtied since the last harvest** (page-table dirty bits) into the selected plane with page-sized copies (not byte loops). A DOOM frame is four plane passes of 16000 bytes (4-5 pages each): 4 harvests of ≤5 pages per frame, no faults. A page flip must not force harvesting untouched planes. Reads from A000 while direct must still see the exact VGA model (read mode 0 through the plane the read-map register selects).
2. **Write modes that need latches** (Wolf3D's write mode 1 copies, `gc[1]`/`gc[3]`/`gc[8]` non-default): decide per mode what can be direct and what stays trapped; measure Wolf3D's pattern from its source and make its dominant path direct too.
3. **Measure before/after** with a host replay of DOOM's frame write pattern (out 3C5 mask; 16000-byte stores; CRTC 0C/0D flip) and Wolf3D's: planes, faults, harvests, harvest µs, present µs per frame, into `docs/validation/`. Extend the `vm_beat_vga`/`vm_perf_video` records with harvest cost and dirty-page counts.
4. The VGA model stays exact; `f3-vm` video cases and the compositor goldens of the DOS window must not change (re-record only with a justified reason).

## Acceptance

Kernel host suites incl. the video/present benches; the lead runs `f3-vm` video cases, `doom-qemu-t23`, `wolf3d-qemu-t23` on QEMU and then the T23: `vm_beat_vga faults` flat during gameplay, DOOM frames/s ≥ 35. Files: `src/kernel/vm/dosvm_vga.c`, `virtual_vga.c`, `vga_presenter.c`, `dosvm_perf.inc` counters. Do not touch `dpmi_dispatch.c`/`v86.c` (f3-15) nor scheduling (f3-17).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f3-16-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
