# f2-36 — Files: "Computer" view with drive letters, retro folder names, hidden system nodes (owner decision 2026-10-12)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-36` (branch `wt/f2-36` from `wt/integ2` f1cedd38). Host-testable code only; the lead runs QEMU.
- **Runs in parallel with f2-35** (`../CiukiOS-wt-f2-35`), which renames `/bin` → `/Programs` and `/home` → `/Documents` everywhere and creates `System` and `Games` as long names. Use the NEW names in all code you write; do not perform the rename yourself. Where you must touch an existing `/bin` or `/home` literal inside Files, write the new name: the lead merges f2-35 first and resolves the overlap.

## Decision (owner, 2026-10-12)

The file tree the user sees follows Windows 98/ME and Mac OS 9: a "Computer" view with drive letters, folders named Programs, Documents, System, Games, and no Unix housekeeping nodes. The POSIX view over the drive VFS (`docs/design/posix-subset.md`) stays for programs: `/` is `C:\`, `/mnt/<letter>` are the other volumes, `/dev` is synthetic.

## Scope (apps/files, libdeskui where generic, the shell's desktop icons)

1. **Computer view.** Files' topmost location is "Computer": it lists the volumes as drives, "C:" for the boot volume (`/`) and "D:".."Z:" for the entries present under `/mnt` (one row each, drive icon; optical volumes get the disc icon when the file-system model exposes the type, otherwise the generic drive icon). The Folders tree starts at Computer → C: → folders. "Up" from `C:\` goes to Computer.
2. **Drive-letter paths.** The address bar, the window title and the status bar show `C:\`, `C:\Programs`, `D:\Games`... One pair of functions maps POSIX paths to display paths and back (`/` ↔ `C:\`, `/mnt/d/x` ↔ `D:\x`, separators, case preserved, trailing separators normalised), unit-tested; typing either form in the address bar works. Internally Files keeps POSIX paths.
3. **Hidden structural nodes.** In `C:\` the entries `dev`, `mnt` and `tmp` are not listed (one table in one place); `/mnt` content appears only as drives in Computer. Nothing else is hidden.
4. **Places.** Computer, Programs (`/Programs`), Documents (`/Documents`), System (`/system`), Games (`/Games`, only when present), with Tango icons (`computer` for the Ciuki computer, folder icons as in the catalog). The desktop icon "Computer" (`apps/desktop/shell.c` `icon_add`) opens Files at Computer; "Documents" opens `C:\Documents`.
5. **Operations** (open, new folder, rename, delete, properties) keep working on the POSIX path underneath; disabled at Computer level (drives are not deletable).
6. **Scripted QEMU cases.** Keep the Files window geometry, menu bar and toolbar layout unchanged so the scripted case `files-qemu-t23` (`tests/suites/f2-desktop.json`, anchors (104,70) and later clicks inside the window) still reaches the same widgets; if a click target moves, update the replay model in `tests/host/desktop/` and say so in the report. The pinned checkpoints marked `PIN-FROM-FIRST-RUN` are re-pinned by the lead.
7. **Host tests.** Files unit/replay tests (`tests/host/desktop/files_test.c`, the host fs model, `test_desktop.py`): Computer listing with one and with two volumes, hidden nodes, path mapping both ways, Up from `C:\`, address typing in both forms, places navigation; render PNGs of the Computer view and of `C:\` to `build/f2-36/*.png` for the lead's review.
8. **Docs.** `docs/desktop.md` Files section (Computer, drive letters, hidden nodes).

## Constraints

clang i686 `-Werror`; no QEMU, no `systemd-run`, no image builds, no commits, no diary entry, no files in the repository root or outside the worktree. Host suites green (`python3 -B -m unittest discover -s tests/host/desktop -t .` after `make CAP= sdk` and `make CAP= desktop`). Use `nice -n 19` for builds and tests: a QEMU campaign runs on this machine.

## Deliverable

`build/codex/f2-36-report.md`: diff summary (file:line), the PNG paths, the verbatim summary lines of the host suites, anything left for the lead.
