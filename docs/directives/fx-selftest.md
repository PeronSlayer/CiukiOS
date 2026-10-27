# fx-selftest: one unattended T23 self-test boot for the hardware-only paths

- **Written:** 2026-10-27 11:55 by the lead. Implementer: Claude agent. Base: main 80f0bc34c or later.
- **Owner decision 2026-10-27:** the owner starts one self-test boot on the T23; it runs about five minutes without interaction and writes a complete report to the disk and the serial port. Manual trials then only judge smoothness and quality.
- **Base to reuse:** the F1-28 unattended hardware sweep (`docs/design/test-architecture.md`, "F1-28"; `scripts/test/physical.py`, `scripts/test/run.py` sweep handling, `src/kernel/probes/`), the loader menu (`N Normal / S Safe / L Log / H Hardware / P Probe`) and `BOOT.CFG` options.

## Scope

1. **Entry.** A loader menu choice or `BOOT.CFG` token (choose and justify; the image ships with it available but off) that starts the self-test; it ends at the normal desktop with a visible "Self-test complete: N passed, M failed" notice, or at a full-screen summary if the desktop cannot start.
2. **Checks, each with a bounded time and a PASS/FAIL/SKIP line with its figures:**
   - serial UART present (loader and kernel probe results), BIOS data area COM ports;
   - display: driver, panel, VBE modes, VBE 4F04 state size and a save/restore round trip, BIOS VBE set and restore of 640x480x8 (Warcraft II), 320x200 mode 13h and 640x480x32 on the panel with the current policy, desktop mode restored;
   - DOS full screen: enter and leave with a built-in test program for mode 13h and VESA 101h, timings, and the return to the window;
   - Savage 3D: the background qualification result (forced to run and awaited), phases and figures; a short GL draw through the hardware path;
   - 2D acceleration counters;
   - optical: drive identity, media state; if a disc is present, TOC, ISO mount and a file read;
   - APM connect/version/capabilities and the ACPI S5 data (never power off), ThinkPad EC;
   - audio: device, a short silent stream through the mixer (no sound), MIDI synthesizer allocation;
   - network: NIC, link, DHCP lease;
   - USB: controllers and devices; storage: ATA modes and DMA;
   - kernel heap and page allocator statistics, any allocation refusals during the run.
3. **Report.** `\System\LOGS\SELFTEST.LOG` (plain text, one line per check, then details) and the same lines on the serial port with the existing record framing; `scripts/test/physical.py` (or a small companion) parses it from a disk copy or a serial capture into a summary.
4. **Safety.** No check writes to the disk outside the log, changes saved settings, powers off or reboots unless the step is the documented last one; every check is skipped cleanly when its device is absent, so the same image works on any supported machine.
5. **Host tests** for the check sequencing, report format, parser and skip behaviour; existing probe and runner tests stay green.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-selftest` (branch `wt/fx-selftest`); never commit, merge or push; never run QEMU or build images; run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-selftest-report.md` in the worktree. Leave changes uncommitted.
