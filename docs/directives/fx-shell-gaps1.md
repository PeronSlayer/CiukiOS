# fx-shell-gaps1: selection and drag-and-drop gaps (DT-4, FM-19, FM-38, DD-3, DD-4, DD-7)

- **Written:** 2026-10-29 11:33 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Inventory:** the rows above in `docs/design/os-feature-inventory.md`, all `absent`. Check each one against current `main` first.

Two parts, each in its own worktree from `main`:

| Part | Worktree id | Report |
|---|---|---|
| A | `fx-select` | `build/codex/fx-select-report.md` |
| B | `fx-dnd2` | `build/codex/fx-dnd2-report.md` |

Before coding, research the Windows 98 shell behaviour: the Resource Kit text under `build/rskt/` (keyboard and mouse tables), Microsoft documentation (List-View, `DoDragDrop`, `IDropTarget`, drag images) and contemporary manuals. Record the sources in `docs/design/file-operations.md` or the desktop design document.

## A. Selection (DT-4, FM-19, FM-38)

1. **Rubber-band (marquee) selection** on the desktop and in every Files icon/list/details view.
   - Dragging from empty space draws the selection rectangle and selects every intersected item.
   - With Ctrl held the rectangle toggles items; with Shift held it adds to the selection.
   - The view auto-scrolls when the pointer leaves it.
2. **Ctrl+click** toggles an item. **Shift+click** selects a range from the anchor, and **Ctrl+Shift+click** extends the range. **Ctrl+A** selects all, on the desktop too.
   - Keyboard focus and the anchor follow the Windows rules; Ctrl+arrows move focus without changing the selection, and Space toggles.
   - Every existing action on a selection (Delete, Cut/Copy/Paste, Send To, Properties, drag) works on the multi-selection.
3. **FM-38.**
   - Shift+click on a folder window's Close button closes that folder and all its parent folder windows.
   - Ctrl+double-click on a folder opens it in a new window, whatever the single-window setting.

## B. Drag and drop (DD-3, DD-4, DD-7)

1. **DD-3, right-drag.** Dropping with the right button shows a menu: Move Here, Copy Here, Create Shortcut(s) Here and Cancel. The bold default follows the left-drag rule for that source and target. This applies on the desktop, in Files, to the Recycle Bin and to folders.
2. **DD-4, drag feedback.**
   - A translucent drag image of the dragged items; the cursor shows the `+` for copy, the arrow for link, and no-drop over a refused target.
   - The drop target is highlighted.
   - The view auto-scrolls near its edges, and hovering over a folder for a moment opens it.
   - Esc cancels the drag. Win32 `DoDragDrop` keeps its existing contract.
3. **DD-7.** When a move's source cannot be deleted (read-only medium, access denied, file in use), the copy is kept, the source is left in place, and a Windows-style message names the files that stayed.

## Tests and evidence

- Add host tests for each item in the existing desktop/Files test modules, covering the mouse and keyboard paths and the error path of DD-7.
- Update the inventory rows.
- Run the desktop and Files host tests sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run` or image builds.
- Do not change the Ciuki portrait or the Tango icons.
