# f4-26 — Savage on the T23 after f4-22: depth programmed yet the fill still writes 0x66 bytes; the idle guard costs on every present and the init blocks the desktop for 10 s

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f4-26` from `main`. Host-testable; the lead tests on the T23.

## Evidence (T23, image ce2e6ce9, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/ce2e6ce9/disk-log/BOOT.LOG.clean`)

- Init records now prove the depth writes: `init reg=00050 wrote=000000f1 read=000000f1` (CR50 bits 5:4 = 11, 32 bpp), `init reg=0816c wrote=10000001 read=10000001` then `wrote=10200401 read=10200401` (GBD2 bpp field 0x20, stride 0x401), `reg=08168 wrote=00000000`, `reg=00031 wrote=0000000c`, CR40 01. Still: `bci_test phase=fill x=1023 y=767 wrote=004a8166` → `result=failed read=66666666 read_valid=1 restored=1`: the engine wrote the low colour byte to all four bytes of the right pixel. With CR50 and GBD2 at 32 bpp, the remaining suspects are the BCI command itself: destination descriptor selection (`BCI_CMD_DEST_GBD` versus the primary bitmap descriptor whose depth is the firmware's 8 bpp), the `SEND_COLOR` format and the solid-fill command bits for the SuperSavage, the pattern/mono expansion flags, the COB/shadow being disabled (f4-19 chose "no COB") and the command word order. Compare byte by byte with xf86-video-savage `savage_accel.c` (`SavageSetupForSolidFill`, `SavageSubsequentSolidFillRect`, `BCI_CMD_*` in `savage_bci.h`) and savagefb `savagefb_accel.c`.
- `[accel] driver=savage ops=1 idle_waits=151906` at the end of the session (47 samples): the quarantined driver's idle guard (f4-19 "fbdev retains quarantined operations as an idle guard after a submitted startup command fails") runs on every CPU present: about 900 idle waits per second with `poll_us=20`, on top of each frame. The owner reports the system "lags more in general".
- `savage quarantined ... us=10142589 trace_us=3224717` and the desktop `stage=entry us=0 → settings_read us=9880000`: the init on the `savage-marker` thread takes 10 s (3.2 s of serial trace plus polls) and the desktop start waits behind it (presenter/console exclusion). The owner sees the wallpaper appear very late.

## Task

1. Make the BCI fill write the full 32-bit colour: fix the command encoding/destination selection per the references; the marker test must read `004a8166` exactly. If the SuperSavage needs the primary bitmap descriptor programmed as well (0x81C0 primary stream base/stride, PBD), do it for the engine only, without touching scanout timing.
2. After a failed engine test the driver is quarantined for the session: one final idle proof, then **no** idle wait on the CPU present path (`idle_waits` must stop growing); the fallback is plain software presentation.
3. The init must not block the desktop: run it fully asynchronous to the desktop start (no presenter exclusion while the desktop reads settings and presents its first frames), and cut the trace: per-register records only on failure or at most 40 records total, no 3 s of serial at boot. Target: desktop `settings_read` within 1 s of `entry` with Savage present.

## Acceptance

`savage2d_test.c`, `test_accel.py`, desktop startup tests green; on the T23: `bci_test result=passed`, `quarantined=0`, ops increasing, `idle_waits` flat, desktop first frame as fast as without Savage. Files: `savage2d.c`, `fbdev.c`, tests. Not `thinkpad_ec.c` (f4-25).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Research the references first where named (`--search`), record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f4-26-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
