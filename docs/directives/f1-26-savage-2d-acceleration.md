# f1-26 — 2D acceleration for the S3 SuperSavage IX/C (ThinkPad T23) with a VBE fallback

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-26` from `main`. Host-testable (register-level model of the Savage 2D engine); the lead qualifies on the T23.

## Owner request

Hardware graphics acceleration has been requested since the start. The first step that pays on the T23 is the 2D engine of the S3 Savage family (SuperSavage IX/C, PCI 5333:8c2e and siblings): BitBLT (screen-to-screen copies for window moves and scrolling), solid fills, pattern fills, and the hardware cursor, driven from the desktop presenter instead of CPU copies through the aperture. 3D (step F7) is separate and not part of this directive.

## Task

1. Research from primary sources (S3 Savage/SuperSavage register documentation, the XFree86/X.Org `savage` driver programming sequences as behavioural reference, no code copying; cite links in `docs/design/savage-2d.md`): engine initialisation (MMIO base from the PCI BAR, GE registers, BCI command stream on Savage4/SuperSavage), 2D commands (bitblt with ROP, solid fill, monochrome pattern), the hardware cursor registers, and the rules for mixing CPU writes to the LFB with engine commands (wait-for-idle before CPU access to a region the engine is writing).
2. Driver `src/kernel/drivers/savage2d.c` on the registry (PCI class 03, S3 vendor, device ids of the Savage4/SuperSavage family; firmware-ownership rules kept; quarantine on any fault or timeout, falling back to the software presenter transparently), with a generic `struct accel_ops` (copy_rect, fill_rect, cursor set/move/show) the presenter uses when available; software fallback keeps full function on QEMU and other cards.
3. Desktop presenter: window moves, scrolls, solid backgrounds and the cursor go through `accel_ops`; everything else stays software with the f1-25 damage pipeline. Counters and a verbose record (`[accel] driver=savage ops=.. idle_waits=.. fallbacks=..`).
4. Tests: register model of the engine (command formatting, idle waits, timeouts, fallback), presenter tests with and without accel giving identical frames.

## Constraints

clang i686 `-Werror`; no QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f1-26-report.md` with the register references and what the lead must verify on the T23 (engine init, a blit, cursor).
