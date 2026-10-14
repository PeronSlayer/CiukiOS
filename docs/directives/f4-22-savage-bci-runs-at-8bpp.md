# f4-22 — Savage BCI executes on the T23 but at 8 bpp: program the 32-bpp depth and keep the init trace within the work budget

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search`. **Worktree:** `../CiukiOS-wt-f4-22` from `main`. Host-testable (`tests/host/savage2d_test.c`); the lead tests on the T23.

## Evidence (T23, image 445e91c7, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/445e91c7/disk-log/BOOT.LOG.clean`, lines 177-424)

f4-19's per-register records all read back as written (3C3=01, 3C2=EF, CR38 48→4A and CR39 A0→A8 are the lock-state echoes, CR40 01→01, SR08 06, SR26 40, CR66 8B/89, ...), status `48c60 raw=07e00000` idle, then **`bci_test phase=fill x=1023 y=767 wrote=004a8166 before=0010243c`** and **`bci_test result=failed error=-5 wrote=004a8166 read=66666666 read_valid=1 restored=1`**. The engine executed the fill: the pixel changed from 0x0010243c to 0x66666666, i.e. the low byte 0x66 of the 32-bit marker replicated four times — the BCI ran the fill as an 8-bit-per-pixel operation on a 32-bpp surface. The depth comes from CR50 bits 4-5 (f4-19 "preserves firmware pixel-length bits": the firmware left the 8-bpp VGA value) and/or the GBD1/GBD2 bits-per-pixel field. Final: `quarantined reason=bci_test_readback_failed us=4180668 reset_us=201321 poll_us=19 trace_us=3264358`, and **`[work] budget exceeded site=c012bd90 us=2458216`**: the per-register trace (serial at 38400 baud) cost 3.26 s inside one work item.

## Task

1. Program the pixel depth for the active mode in every place the SuperSavage reads it: CR50 bits 4-5 (00=8, 01=16, 11=32 bpp on Savage4-series per xf86-video-savage `SavageSetGBD_*` and savagefb `SavageSetup2DEngine`/`savage_set_par_int`: verify the encoding in those sources and in RE046-B), the GBD1 bpp field and stride, CR67 if the references set it for the 2D engine; keep everything else of f4-19. The marker test must then read back `004a8166` exactly (and 16-bpp modes their packed marker).
2. Keep the init trace but move it out of the work budget: emit the per-register records only from the init path at boot (not inside the work item) or rate-limit them to the first initialisation and batch them after the engine test; the work budget record must not fire.
3. Host model: a SuperSavage model that applies fills at the depth selected by CR50/GBD, so the 8-bpp replication is reproduced and the fix proven; records asserted.

## Acceptance

Host suites green; on the T23: `bci_test result=passed`, `[accel] driver=savage ... quarantined=0`, ops increasing during desktop use, no `[work] budget exceeded` from the Savage site, panel stable with the approved Ciuki portrait colours. Files: `savage2d.c`, `fbdev.c`, `savage2d_test.c`. Do not touch the USB stack (f1-59) or `thinkpad_ec.c` (f4-21).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first (`--search` is enabled), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-22-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
