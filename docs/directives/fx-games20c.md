# fx-games20c: installed trees of the twenty DOS titles

- **Written:** 2026-10-28 20:18 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: branch `wt/integ8`, which contains fx-games20 and fx-games20b.
- **Inputs:**
  - `build/codex/fx-games20b-report.md` in `../CiukiOS-wt-fx-games20b`, "Installed trees" table;
  - `scripts/test/dos_games.py`, `scripts/test/dos_titles.py` and `config/dos-titles.json`;
  - the pinned archives in `/home/peronslayer/Desktop/CiukiOS/build/downloads/dos-titles/`. They are read-only and never committed.

## Lead decision G20-1

Like a Windows 98 user, the acceptance installs each title with its original installer wherever the package has one, then runs the game's own setup program for sound. Host extraction is allowed only where the package has no installer (direct ZIP game files) or as a cross-check of the installer's output.

## Scope

1. **Host extraction tool.** Add a `dos_games.py extract --title T` action. It extracts the payload of the pinned archive (ZIP, numbered parts, Apogee SHR, nested SFX) into a scratch directory under `build/`.
   - It is a read-back reference for the installed tree, never an installed tree by itself.
   - Cover all eighteen extractable titles.
   - Descent and Descent II are excluded (segmented SOW members).
2. **Installer and sound-setup acceptance steps.** For each title, write the acceptance-case steps that:
   - run the original installer in a DOS VM from the desktop path to its exit, into `C:\GAME\<title>`;
   - run the title's sound setup program (SETUP.EXE, SETSOUND, the game's own sound menu) to select Sound Blaster/AdLib or General MIDI as the CiukiOS DOS VM offers them (`docs/design` DOS sound contract: SB16 at 220h IRQ 5 DMA 1/5, MPU-401 at 330h).

   Take every key sequence from the package's own manual or readme, cited per title. Never guess menu positions. Where the manual does not settle them, mark the step `needs-lead-calibration` with the screen to check.
3. **Pinning the installed tree.**
   - After the lead's QEMU run, `pin-installed` records the tree the installer produced.
   - Add a `compare-installed` check against the extraction reference: same game data files, installer-only and config files listed.
4. **Host tests** for the extractor, using small synthetic archives only, and for the case steps.

## Acceptance

- The host tests pass.
- `dos_games.py validate` and `bash scripts/test/acceptance.sh --list` pass.
- No game bytes enter the repository.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-games20c` (branch `wt/fx-games20c`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/fx-games20c-report.md`. Leave the changes uncommitted.

## Lead decision G20-2 (2026-10-28 20:48)

The installers and sound-setup steps use the resources that the VM's `BLASTER` variable declares. The CiukiDOS factory setting is `A220 I7 D1 H5 T6` (SB16 at 220h, IRQ 7, DMA 1/5, MPU-401 at 330h), matching `src/kernel/vm/dosvm_audio_state.h`. A Windows 98 PC's game setup likewise takes the installed card's resources.

- Where a setup program asks for the IRQ, the step selects 7.
- The global default does not change.
- The title battery's IRQ 5 text is corrected to read `BLASTER`.
