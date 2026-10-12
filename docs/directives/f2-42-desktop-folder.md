# f2-42 — The desktop is a folder: new folders and files on the desktop

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-42` from `main`. Host-testable (shell harness, replay); the lead qualifies on QEMU/T23.

## Owner report (2026-10-13)

"Sul desktop non hai la possibilità di creare cartelle."

## Task

1. The desktop shows the contents of `C:\Documents\Desktop` (created by the image builder; `/Documents/Desktop` in the POSIX view) after the fixed system icons (Computer, Documents, Trash): folders and files with their catalog icons and labels, in the icon grid, positions persisted in `desktop.cfg` (per name) with Arrange Icons resetting them.
2. Right-click on the desktop: New > Folder, New > Text Document, New > Bitmap Image (CiukPaint), Paste; on an icon: Open, Open With (catalog programs for the extension), Rename (inline edit like Files), Delete (to Trash), Properties; drag between desktop and Files windows (move/copy with the existing Files model), double-click opens with the catalog program, folders open in Files.
3. Keyboard: F2 rename, Delete, Enter open, arrows between icons.
4. Files: `Desktop` appears in Places and in the tree under C:\Documents.
5. Tests: shell harness for the folder listing, new folder/file creation with unique names (New Folder, New Folder (2)), rename validation (FAT rules), delete to Trash, position persistence, replay of a scripted New Folder; renders to `build/f2-42/*.png`.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f2-42-report.md`.
