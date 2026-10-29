# fx-accessories1: Clipboard Viewer (AX-10) and Quick View (FM-32)

- **Written:** 2026-10-29 11:34 by the lead. Implementer: Antigravity, `claude-sonnet-5-5-medium`. One session per part:

  | Part | Worktree id | Report |
  |---|---|---|
  | A, Clipboard Viewer | `fx-clipview` | `build/codex/fx-clipview-report.md` |
  | B, Quick View | `fx-quickview` | `build/codex/fx-quickview-report.md` |

- **Inventory:** rows AX-10 and FM-32 in `docs/design/os-feature-inventory.md`, both absent.
- **Model to follow:** the existing native accessories in `apps/accessories/` and `apps/systools/`: the app structure, `libdeskui` controls, Start menu registration, the `apps/build_apps.py` entries, and their host tests in `tests/host/desktop/`. Reuse the shared clipboard, file-type and viewer code; do not duplicate decoders.

Before coding, research the Windows 98 behaviour of each part: the Resource Kit text under `build/rskt/` ("Clipboard Viewer", "Quick View") and Microsoft documentation. Record the sources in the accessory's design note.

## A. Clipboard Viewer

A native accessory under Programs ▸ Accessories ▸ System Tools, as in Windows 98.
- It shows the current clipboard content and updates when the clipboard changes; use the existing clipboard-change notification.
- The Display menu lists the available formats and shows the selected one. Text, Unicode text, bitmap/DIB, metafile and enhanced metafile are rendered through the existing GDI/clipboard code; other formats are listed and greyed out.
- File ▸ Open and Save As use the Windows `.CLP` format; follow its documented layout, and record the source.
- Edit ▸ Delete empties the clipboard after a confirmation.
- Use the existing Tango icon family for its icon. Do not draw a new icon.

## B. Quick View

- A "Quick View" entry in the context menu of the file types it can preview, in Files and on the desktop.
- It opens a read-only viewer window:
  - text files (TXT, INI, BAT, LOG and source code) with a fixed font;
  - images (BMP, PNG, JPEG, GIF) through the existing image decoder;
  - a summary for executables (PE/MZ header: type, subsystem, version resource if present).
- The window has "Open File for Editing" (the registered default action), Increase/Decrease font and Page view for text, and accepts dropping another file onto it.
- Files larger than a bounded size are previewed only partially, with a notice.

## Tests and evidence

- Add host tests for each part in the existing desktop test modules:
  - A: format listing, change notification, `.CLP` save/open round trip, Delete;
  - B: context-menu presence by type, text/image/executable preview, large-file bound, drop.
- Update the inventory rows.
- Run the affected host tests one at a time with `nice -n 19`.

## Rules

- Follow `build/lead-scratch/agy-rules.txt`.
- Never run QEMU, `systemd-run` or image builds. Never commit.
- Do not change the Ciuki portrait or the Tango icons.
