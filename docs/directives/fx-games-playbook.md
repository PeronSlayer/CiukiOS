# fx-games-playbook: draft installer and sound-setup playbooks for the 20 DOS titles

- **Written:** 2026-10-29 11:07 by the lead. Implementer: Antigravity, `gemini-3.8-flash-high` (light, host-only research). Two sessions:

  | Session | Titles | Worktree |
  |---|---|---|
  | A | doom, quake, wolf3d, rott, keen4, duke3d, heretic, hexen, descent, descent2 | `/home/peronslayer/Desktop/CiukiOS-wt-fx-games-pbA` |
  | B | shadowwarrior, wacky, cosmo, duke2, raptor, hocus, biomenace, blakestone, terminal, jazz | `/home/peronslayer/Desktop/CiukiOS-wt-fx-games-pbB` |

- **Purpose:** the acc-t23 per-title cases do not run until the lead has run each original installer and sound setup in the CiukiOS DOS VM, pinned the installed tree and calibrated the checkpoints (`docs/validation/fx-games20.md` steps 3–5; `tests/dos-titles/install20.json` steps marked `needs-lead-calibration`). A precise playbook per title lets the lead drive those QEMU runs with scripted keys instead of exploring each installer by hand.

## Scope (host only, no DOS execution)

For each title in your session:

1. **Reference tree.** Run `python3 -B scripts/test/dos_games.py extract --title ID` (host only; it refuses an existing destination). Record the resulting file list summary.
2. **Read the original package documents** inside the pinned archives in `build/downloads/dos-titles/`, with `unzip -l` and `unzip -p`, read-only: README, INSTALL/SETUP text, ORDER/LICENSE, manuals. Use `strings` on INSTALL.EXE and SETUP.EXE only to recover menu texts.
3. **Write the playbook section** for the title in `docs/validation/fx-games20-playbook-<A|B>.md`:
   1. the installer command and every expected screen, in order, with its exact on-screen text where the documents or strings give it, and the keys to press;
   2. the destination `C:\GAME\<DIR>` as in `install20.json`;
   3. the sound setup: the program, the menu path to SB16 (Sound Blaster 16) at 220h, IRQ 5, DMA 1, high DMA 5, music on OPL/AdLib, and General MIDI on MPU-401 330h where the title supports it (Descent II music case). The target choices come from `docs/validation/fx-games20.md`; never assume menu indices without a source;
   4. the files the setup writes (for example DEFAULT.CFG, SETUP.CFG, SOUND.CFG);
   5. the game's `enter`/`play`/`quit` keys from `tests/dos-titles/games20.json`, checked against the manual;
   6. for IPX titles, the real network driver executable (for example IPXSETUP.EXE, SETUP's network menu), its host and join arguments and the input, from the documents;
   7. a confidence mark per step: `documented` (quote the source member and line), `strings` (recovered from the binary) or `unknown` (the lead must observe it).
4. **Session A only, hexen terms:** `terms_reviewed` is false for hexen. Read the demo package's exact licence and readme. Record its redistribution clauses verbatim, with member names and SHA-256, in a section "hexen terms (draft for the lead)". Do not change `terms_reviewed`; the lead decides.

## Rules

- Follow `build/lead-scratch/agy-rules.txt`.
- Do not edit `tests/dos-titles/*.json`, scripts or product code.
- Never run DOS programs, DOSBox, QEMU or installers.
- Never copy game data into tracked files. Quote only short menu texts and licence clauses.
- Write your summary report to `build/codex/fx-games-pb<A|B>-report.md`.
