# Shell drag and drop and rubber-band selection (fx-19)

Written 2026-10-23 19:03 CEST for directive fx-19
(`docs/directives/t23-e831e685-trial-fixes.md`). Owner: drag and drop must
work everywhere; rectangle selection must work on the desktop and in Files.

## Why the earlier implementation did not hold

f2-42 (`desktop-folder.md`) implemented a single path: Files published its
selection **to the shared file clipboard** and asked the desktop to drop "at
the current pointer" on release. Consequences seen in the T23 trial:

- no drag image, no cursor feedback and no drop-target highlight while
  dragging (the desktop did not know a drag was in progress);
- every drag overwrote the user's Cut/Copy clipboard;
- a plain drag always moved, even across drives (the trial's
  `[files] job=move` from E: to C:), Ctrl always copied, no Shift, no
  right-drag menu, no Esc;
- the desktop had a single selected icon, so nothing could be dragged or
  deleted as a group, and no rectangle selection existed anywhere;
- in Files a press on an already selected item collapsed a multi-selection
  before it could be dragged;
- no application accepted dropped files.

## Reference behaviour (Windows 95/98)

Sources:

- OLE drag loop: [DoDragDrop](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-dodragdrop),
  [IDropSource::QueryContinueDrag](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-idropsource-querycontinuedrag)
  (Esc cancels, releasing the button drops),
  [IDropSource::GiveFeedback](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-idropsource-givefeedback)
  (default move/copy/link/no-drop cursors),
  [IDropTarget::DragOver](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-idroptarget-dragover)
  (the target reports the effect for the key state; DragOver repeats while
  the pointer rests) and
  [DROPEFFECT](https://learn.microsoft.com/en-us/windows/win32/com/dropeffect-constants)
  (NONE 0, COPY 1, MOVE 2, LINK 4).
- Drag threshold: [GetSystemMetrics SM_CXDRAG/SM_CYDRAG](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getsystemmetrics)
  ("pixels on either side of a mouse-down point", default 4) and
  [DragDetect](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-dragdetect).
- Shell data transfer, right-drag and the Recycle Bin:
  [Handling Shell Data Transfer Scenarios](https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios)
  ("Support left and right drag", "Use standard feedback cursors").
- Default effect: Raymond Chen, [Will dragging a file result in a move or a copy?](https://devblogs.microsoft.com/oldnewthing/20041112-00/?p=37323):
  Ctrl+Shift creates a shortcut, Shift moves, Ctrl copies; with no modifier
  the same drive moves and another drive copies. Alt also requests a link
  in the 9x shell (secondary sources; treated the same way here).
- [DragAcceptFiles](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-dragacceptfiles)
  and [WM_DROPFILES](https://learn.microsoft.com/en-us/windows/win32/shell/wm-dropfiles):
  Notepad, Paint and the media player open what is dropped on them.
- Mouse and selection conventions: [Windows UX guide, Mouse and Pointers](https://learn.microsoft.com/en-us/windows/win32/uxguide/inter-mouse)
  (Shift extends contiguously, Ctrl toggles, the context menu follows the
  button release, Esc abandons a drag, capture the mouse while dragging).
- Windows 95/98 MS-DOS Prompt windows pasted the dropped file's path
  (informal tip collections only).

## Expected behaviour, implemented

### Selection

| Gesture | Desktop | Files (Large Icons, List, Details) |
| --- | --- | --- |
| Click | selects only that item, sets the anchor | same |
| Ctrl+click | toggles; on an already selected item the toggle waits for the release so the selection can be dragged | same (`DU_SELECT_PENDING` / `DU_SELECT_RELEASE`) |
| Shift+click | selects every icon inside the rectangle spanned by the anchor and the item (icon view rule); Ctrl+Shift adds | index range from the anchor (fm_select) |
| Press on a selected item | keeps the whole selection; a release without a drag then selects only it | same |
| Press on empty space | clears (unless Ctrl/Shift) and starts a rubber band | same; in Details the row outside icon+name is empty space (Explorer 98) |
| Rubber band | dotted XOR outline; items touched by the band are selected; Ctrl toggles them against the selection at the press, Shift adds | dotted band, scroll-independent anchor, auto-scroll outside the body (50 ms toolkit timer) |
| Ctrl+A / arrows / Shift+arrows / Ctrl+arrows / Ctrl+Space | all icons / nearest icon in that direction / extend from the anchor / move the focus only / toggle the focus | existing list keys |
| Enter, double-click | opens every selected item | opens the item |
| Delete, Ctrl+X/C | the whole selection (one Trash job, a free name per item) | existing commands |

### Drag session (desktop compositor owns it)

1. A source begins after the pointer leaves the 4-pixel square around the
   press (left or right button). Files saves the data object
   (`/system/settings/files.drag`, `X` then one `D|F` + path line per
   item; never the clipboard) and sends `DESK_DRAG_BEGIN`; the desktop
   accepts it only from the focused, desktop-launched Files window that
   holds that button. Desktop icons start the session in the shell.
2. While the session runs no client receives motion. The shell evaluates
   the object under the pointer on every motion, key change and 100 ms
   repeat:
   - desktop background: the Desktop folder (desktop source: reposition by
     the pointer offset, Ctrl copies beside the originals as "Copy of");
   - a folder icon, Documents or Pictures: that folder; the Trash: move
     only (Ctrl refused); Computer, documents and Win32 links: no drop;
   - a Files window: `DESK_DRAG_OVER` (client x/y, keys); the window answers
     `DESK_DRAG_STATUS` (effect, allowed set) after hit-testing its list,
     folder tree or Places; it highlights that item, auto-scrolls in the
     16-pixel edge zone and expands a collapsed tree folder after one
     second; the cursor shows no-drop until it answers;
   - a window that called `du_window_accept_files` (`DESK_ACCEPT_FILES`):
     the copy cursor; the drop sends `DESK_DROP_FILES` and activates it;
   - a taskbar button: no drop; resting one second restores and activates
     its window; a drop there explains this (the Windows 98 message);
   - the top bar, frames and other windows: no drop.
3. The effect comes from `fm_drop_effect` (apps/files/model.c), the one
   shared rule, restricted by what the source and target allow (CD sources
   copy only, CD targets accept nothing, no native shortcuts so LINK is
   never allowed) and by `fm_drop_check` (no folder into itself; moving
   into the folder that already holds every item is a no-op).
4. Feedback: the translucent drag image (desktop source: the dragged icons
   at their offsets; Files source: up to four icon + name rows and "and N
   more"), the target highlight, and the cursor `DESK_CURSOR_DRAG_MOVE`,
   `_COPY`, `_LINK` or `DESK_CURSOR_FORBIDDEN`.
5. End: releasing the left button drops with the current effect
   (`DESK_DRAG_DROP` to a Files target, or a desktop folder job through
   `desk_folder_transfer`, items placed at the drop point); releasing the
   right button opens Move Here / Copy Here / Create Shortcut(s) Here /
   Cancel with the plain-drag effect in bold and unavailable entries
   disabled; Esc, a dismissed menu, an input resync or the source window
   closing cancel. The source gets `DESK_DRAG_END` with the performed
   effect; after a move it refreshes its folder for up to ten seconds.

### Applications accepting files

| Application | Drop behaviour |
| --- | --- |
| CiukNote | opens the first file, after the unsaved-changes question |
| CiukPaint | opens the first picture, after the unsaved-changes question |
| Music Player | a dropped file plays now; a dropped folder replaces the playlist; further items are added |
| Image Viewer | shows the first picture |
| DOS Box | does not accept drops: see gaps |

## Protocol (v2 appendix, `apps/desktop/protocol.h`)

`DESK_DRAG_BEGIN`=42, `DESK_DRAG_OVER`=43, `DESK_DRAG_STATUS`=44,
`DESK_DRAG_DROP`=45, `DESK_DRAG_END`=46, `DESK_ACCEPT_FILES`=47,
`DESK_DROP_FILES`=48; cursors 22–24. Paths never travel in messages. The
oracle rows are in `tests/host/abi_layout_test.c`; `docs/developer-api.md`
is regenerated.

## Remaining gaps (explicit)

- Shortcuts (LINK): CiukiOS has no native file shortcut type, so Ctrl+Shift
  and Alt show no-drop and "Create Shortcut(s) Here" is disabled; dragging
  drives out of Computer (link-only in Windows) is not offered.
- DOS Box: the Windows 9x DOS prompt pasted the dropped file's short path.
  CiukiOS DOS sessions see private managed drives (unified DOS contract:
  managed C:, D:, E:), not host paths, and no 8.3 alias API exists, so a
  pasted path would be wrong; the DOS window does not accept drops.
- Dropping onto a program or Win32 link icon to open the file with it, and
  dropping onto the Run dialog, are not implemented.
- Drops are not accepted by Win32 (v3) windows: no OLE/WM_DROPFILES bridge
  to the Win32 layer yet.
- Desktop selection is not kept when the folder is re-listed (as before).
