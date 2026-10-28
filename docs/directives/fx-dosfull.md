# fx-dosfull: DOS games start full screen by default, as Windows 98 does

- **Written:** 2026-10-27 21:50 by the lead. Implementer: Claude agent. Base: main 9e2df1875 or later.
- **Owner decision 2026-10-27:** every DOS game starts full screen by default; Alt+Enter switches to a window; the choice is remembered per program.
- **Research (lead):** Windows 98 Resource Kit, chapter 25 "Running Games" (local `build/rskt/wrkc25_3.htm`): "If you run a game that uses graphics modes, and Windows 98 fails to run it in a full screen, press ALT+ENTER. To run the game in a full screen every time you start it ... Screen tab ... Usage ... Full-screen"; Windows 98 applies per-program settings from Apps.inf without user configuration; Alt+Enter toggles and Windows remembers the mode a program last used. In a window every VGA register access traps (the T23 DOOM log: 50-75 thousand port 0x3C5 exits per second); full screen runs the game on the hardware.

## Scope

1. A per-program PIF-style setting (Screen usage: full screen or window), stored with the program's DOS settings (per user, within sec-k05's model when it lands; today the existing DOS Box/launcher settings), defaulting to **full screen for every DOS program that sets a graphics mode** (Games menu entries, DOS Box launches of `.exe/.com/.bat`, Files double-clicks); text-mode-only programs and the DOS prompt keep starting in a window.
2. The first graphics mode set enters full screen automatically through the existing full-screen path (`dosvm_fullscreen.c`, BIOS VBE state on panels from fx-biosvm2, native SuperSavage where proven); Alt+Enter toggles in both directions; the last mode used is remembered per program, as Windows 98 does.
3. When full screen is refused (QEMU standard VGA, f3-29; a quarantined BIOS video family), the game continues in the window with no error dialog, the refusal is logged once, and the setting is not changed.
4. The program Properties dialog (DOS Box/Files) shows the Screen usage choice like Windows 98's Screen tab.
5. Tests: launcher and settings host tests, the DOS VM full-screen replays, and the acceptance suite's DOS case updated (games start full screen on hardware-like profiles, fall back to the window on QEMU's refusal).

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-dosfull` (branch `wt/fx-dosfull`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-dosfull-report.md` in the worktree. Leave changes uncommitted.
