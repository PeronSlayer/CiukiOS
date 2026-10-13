# f1-27 — Savage 2D engine initialisation stalls the T23 boot: make it correct, late and bounded, then enable it again

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search` (S3 Savage4/SuperSavage register documentation, X.Org `savage` driver as behavioural reference for the MMIO base per chip generation, no code copying). **Worktree:** `../CiukiOS-wt-f1-27` from `main`. Host-testable (engine model with a stuck clock and dead MMIO); the lead qualifies on the T23 with an `L` capture.

## Evidence (T23, 2026-10-13, image 8845b848 / hardware 62e4dc6c)

Both the `N` and `L` boots stop right after `[init] flag safe_mode=0 boot_flags=00000a08 activation_seq=1`; no `[init] framebuffer` line follows. `fbdev_init()` (src/kernel/drivers/fbdev.c ~175) called `savage2d_init(&device)` during the framebuffer activation; the lead made that call compile-time opt-in (`CIUKI_ACCEL_SAVAGE`, commit 03bf0427) as a stopgap. The owner wants the driver working, not disabled. The adapter is the SuperSavage IX/C, PCI 5333:8c2e (two `[display] mtrr wc` lines show the LFB at e8000000, 4 MiB WC range).

## Findings of the independent review (lead and Opus agent, 2026-10-13, `../CiukiOS-wt-savage-review/build/savage-review.md`, branch `wt/savage-review`)

Read that report first and merge its patch (`git merge wt/savage-review` is done by the lead; its files may already be in this worktree: breadcrumb records `[accel] savage probe/mapped/step op=.. reg=..` and the stalled-clock/dead-aperture host tests). Facts established:

- Every software loop on the init path terminates (`wait_idle` has a 100000-poll cap; `g_ticks` and `timing_calibrate()` run before `drivers_init()`), serial output precedes the screen, and no `[accel]` line appears: the CPU stopped inside one register access that never completed. An unmapped BAR read returns all-ones, it does not hang.
- For the SuperSavage IX/C (8c2e) the register window is **BAR0 + 0, 512 KiB**, which the driver maps uncached and correctly; "BAR + 16 MiB" is the Savage3D/MX/IX layout and must NOT be used for this chip. PCI memory-space enable is checked. CR38 = 48h / CR39 = A5h unlock is correct.
- The most likely stall is the **first read of the engine status register 0x48c60**: X.Org's savage driver documents a chip bug that can lock the bus on that read, and ThinkWiki lists the ThinkPad T20–T23 among the affected machines; the driver reads it right after CR66 without resetting the engine. X.Org and Linux savagefb pulse **CR66 bit 1** (and Linux also CR3F bit 3) before using the engine. Second candidate: writes to 0x8128–0x8134 or the descriptors 0x8168/0x816c while the command interface is active (register manual RE046-B).

## Required changes

1. Before the first status read: unlock (CR38/CR39), select display pipe 1 (SR26 = 40h), set the CRTC to 3D4 (MISC bit 0), then pulse CR66 bit 1 (about 10 ms set, 10 ms clear), as the reference drivers do; only then read 0x48c60 **sparsely** (short delay between polls, a cap of a few hundred reads, never a tight loop of 100000).
2. Keep the engine descriptors untouched while the command interface is active (check the BCI/CI state and disable it first per RE046-B), then program them.
3. Log the raw 0x48c60 value once before choosing the idle mask (the manual and the two reference drivers disagree on the idle bits); quarantine on contradictions.
4. **Persistent attempt marker**: write `accel=attempt` into the settings volume before touching the engine and clear it after success; a boot that finds the marker set skips the accelerator (software presenter) and logs `[accel] savage skipped after a failed attempt`, so a bus lock can never brick the boot twice. Initialise late (a kernel work item after storage is mounted and timers run), never inside `fbdev_init()`.
5. Keep the breadcrumb records of the review in verbose boot; always emit one summary line (`ready` or `quarantined reason=..`).
6. Remove the compile-time gate so the accelerator is on by default with these guards.

## Tests (host)

Engine model variants: stuck clock, dead MMIO (0xFFFFFFFF), MMIO base at the wrong offset, locked CRTC registers, engine answering late; the boot path reaching the desktop with each; the records; existing `savage2d_test`, `fbdev_test` and presenter equivalence tests green.

## Constraints

clang i686 `-Werror`; no QEMU, no commits, no diary; `nice -n 19`, one host suite at a time; use `rg` not semble. Report `build/codex/f1-27-report.md` with the register references, the exact init sequence programmed and what the lead must read in the next `L` capture.
