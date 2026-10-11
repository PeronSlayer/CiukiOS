# Directive f2-31: desktop UI toolkit (`libdeskui`) and the Files application

- **Step:** F2+ (desktop applications towards 0.8 parity). **Contracts:**
  `f2-acceptance.md` and `docs/desktop.md` (client protocol v2 of f2-30:
  HELLO, CREATE_WINDOW with a surface, DAMAGE, CONFIGURE, KEY/MOTION/BUTTON,
  CLOSE_REQUEST/CLOSE, PING/PONG; focus and held-input releases),
  `posix-subset.md` (files, directories through `getdents`, `stat`,
  `rename`, `unlink`, `mkdir`, `rmdir`, `ciuki_spawn`), `AGENTS.md`
  identity rules (Tango icons from the pinned archive, English UI copy).
  Reference for parity: the 0.8 Files application (menus File/Edit/View/
  Go/Help, toolbar, address bar, Places and Folders panes, Details/Icons/
  List views, selection model, context menus, operations, keys); its
  legacy description is in `legacy/CiukiOS-docs-legacy-2026-10-09.zip`
  under `docs/desktop-apps-2026-09-29.md` (extract to `build/` to read).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f2-deskui-files`, from `main` after f2-30 is merged.
  Files: new `apps/libdeskui/` (static library built with the SDK's
  `ciuki-cc`, MIT like the desktop code), new `apps/files/`, a generalized
  `apps/build_apps.py` (or an extension of `apps/desktop/build_desktop.py`)
  that builds every application with the same ELF checks and manifest, and
  `scripts/build_image.py` adding `/bin/files`; icons through f2-30's
  `convert_icons.py` and `shell-icons.json` from the pinned Tango archive;
  host tests under `tests/host/desktop/` (toolkit widgets and the Files
  model against a fake directory tree, pixel digests of rendered widgets);
  one QEMU case `files-qemu-t23` in `tests/suites/f2-desktop.json`
  (scripted actions and `desktop_screen` digests like `desktop-shell-*`);
  `docs/desktop.md` (toolkit section).

## Scope

1. **Toolkit.** Window client glue (handshake, surface, damage batching,
   event loop with timers, close request handling, focus and release
   semantics), integer-only drawing with the compositor's 8×16 font,
   widgets: menu bar with drop-down menus and accelerators, toolbar
   buttons with Tango icons, address bar (single-line edit), list view
   with Details (sortable columns), Large Icons and List modes, tree view,
   scrollbars, status bar, splitter, message box, input box, file open/
   save dialog, progress dialog with Cancel; keyboard navigation and a
   per-process clipboard; deterministic rendering (digests in tests).
2. **Files.** Menus File/Edit/View/Go/Help; toolbar Back/Forward/Up/Cut/
   Copy/Paste/Delete/Properties/Views; address bar; Places (the system
   volume and `/bin`, `/system`; mounted volumes as the VFS enumerates
   them) and Folders tree; Details/Icons/List views sorted by Name/Size/
   Type/Modified; selection by click, Ctrl/Shift click, Shift+arrows,
   Ctrl+A, Invert, type-to-find; context menus on item and background;
   operations: New Folder, New Text Document, rename in place, Copy/Cut/
   Paste (files and folders, background job with progress and Cancel,
   replace prompt Yes/Yes to All/No/Cancel, timestamps and attributes
   kept), Delete with confirmation (permanent; the Recycle Bin is a later
   directive), Properties (type, location, size, contents, attributes);
   opening: text files launch `/bin/ciuknote <path>` when present, ELF
   programs run through `ciuki_spawn`; keys Enter, Backspace, Alt+Left/
   Right/Up, F2, F5, Del, Alt+Enter, Alt+D/F4, Ctrl+C/X/V. Drag and drop,
   removable media and the Recycle Bin are explicitly later.
3. **Tests.** Host: widget layout and hit-testing, list sorting, the
   Files model (navigation, selection, copy/move/delete jobs with a fake
   VFS and injected errors), rendering digests. QEMU: `files-qemu-t23`
   launches Files from the Ciuki menu, navigates to `/system`, creates and
   renames a folder, copies a file, deletes it, with `desktop_screen`
   digests at checkpoints; the case is deterministic (fixed clock text,
   fixed window placement).

## Acceptance by the lead

Host tests; SDK/app/image build with `/bin/files` in the manifest; QEMU
`f2-desktop` unchanged plus `files-qemu-t23` PASS; a runner screenshot of
Files for the owner; `f2-all` unaffected.
