# fx-files-nav: Files navigation and view gaps (FM-2, FM-3, FM-4, FM-6, FM-9, FM-12)

- **Written:** 2026-10-29 09:24 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-files-nav`, branch `wt/fx-files-nav` from `main`.
- **Inventory:** the rows in `docs/design/os-feature-inventory.md`. Check each row's evidence against current `main` first; some gaps may already be closed.

## Required behaviour

Before coding, research the Windows 98 Explorer behaviour of each item: Microsoft documentation, the Windows 98 Resource Kit text under `build/rskt/`, and contemporary manuals. Record the sources in `docs/design/file-operations.md` or the Files design document.

1. **FM-3.**
   - View ▸ Arrange Icons by Name, Type, Size and Date.
   - Auto Arrange (a persistent toggle per folder view).
   - Line Up Icons in the icon views.
2. **FM-4.** The address bar keeps a drop-down history of typed and visited locations, at most 25 entries, kept per user. It also autocompletes paths from the file system as the user types: a completion list, arrows to choose, Enter to accept, Esc to dismiss.
3. **FM-6.**
   - Back and Forward each have a drop-down list of their history entries; choosing one jumps there and adjusts both stacks as Explorer does.
   - Backspace goes Up.
4. **FM-9.** The toolbar gets an Undo button wired to the existing one-level Undo (FM-17), disabled when nothing can be undone. Map Network Drive and Disconnect also go on the toolbar, but only if a network-drive service exists on `main`; otherwise record them as remaining.
5. **FM-12.** Renaming a file so that its extension changes asks for confirmation, with Windows 98 wording and buttons ("If you change a file name extension, the file may become unusable…"). Show it only when extensions are visible and the extension really changes.
6. **FM-2.**
   - Details column widths persist per folder view across navigation.
   - Double-clicking a column divider auto-fits that column to its widest visible entry.

## Tests and evidence

- Add host tests in the existing Files/desktop test modules for each item, covering the keyboard and mouse paths.
- Update each inventory row's status and evidence, and `fx-20` if it tracks these rows.
- Run the Files and desktop host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Write the report to `build/codex/fx-files-nav-report.md`.
