# f3-32 — Native SuperSavage mode switch for DOS full screen on the T23 (replacing the refusal of f3-26)

- **Model/effort:** `gpt-6.1-sol`, `xhigh` (Astra only if Sol fails twice). **Worktree:** `../CiukiOS-wt-f3-32` from `main`.

## Evidence

f3-26 refuses full screen on PCI 5333:8c2e because the BIOS VM path left the panel black and the firmware backend quarantined (`docs/directives/f3-26-*.md`, `docs/design/savage-2d.md`); the owner wants DOOM and Quake full screen on the T23 (decision of 2026-10-14). The references for a native mode set exist: the X.Org savage driver (`build/downloads/savage/savage_driver.h`, `savage_accel.c`; download `savage_driver.c`, `savage_streams.c` and `savage_vga.c` from the same repository: they program the SuperSavage/IX mobile chip directly: `SavageWriteMode`, `SavageSave`/`SavageRestore`, stream processor `SavageInitStreams`/`SavageStreamsOn/Off`, panel/LCD expansion `SavageSetPanel`, CR67/CR69, PLL `SavageCalcClock`, VGA registers through the standard ports), the Linux framebuffer driver (`savagefb_driver.c`: `savagefb_set_par`, `savage_set_fixed`/`SavageSetup2DEngine`, panel handling for `S3_SUPERSAVAGE`), and the vendor Windows driver in the pack (`Driver/D/A0400/SSAV1014.INF`, `A0500/ssav1179.inf` for registry options such as panel expansion and stream modes).

## Task

1. Implement a native mode provider for the SuperSavage in the display service: save the complete state the references save (VGA regs, extended CR/SR, streams, panel, PLL), program 320×200×8 (mode 13h) and 640×480×8 for the DOS session with the panel expansion the firmware uses for the desktop mode, and restore the desktop mode exactly, all with the register model on host; the BIOS VM is no longer used for the switch on this chip; the f3-26 refusal becomes a fallback only when the provider reports an unsupported state.
2. Keep the f3-26 safety: checkpoints before each step, readback with masks from the references, watchdog restore, black box.
3. Full-screen transaction integrated with f3-20's VGA transport (port grants, A000 aperture); DOOM's mode 13h/Mode Y and Quake's 320×200 must present; Alt+Enter back restores the desktop within 200 ms.

## Acceptance

Kernel host suite green (register model of the SuperSavage mode set and restore from the references' sequences); QEMU full-screen cases still green with the standard VGA provider; on the T23: DOOM full screen visible and the return to the desktop clean, recorded step by step.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel and the DOS image before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/`. Owner's standing rule: ground every hardware decision in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/`) and in upstream open sources found on the internet; cite them; never program registers from reasoning alone. Report `build/codex/f3-32-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
