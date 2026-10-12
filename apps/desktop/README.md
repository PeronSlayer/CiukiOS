# Ciuki desktop (f2-08)

An ordinary SDK-built ring-3 compositor. The supervisor owns launch, exclusive
grants and the desktop's separate process group. See
[the desktop guide](../../docs/desktop.md) for wire layout, launch arguments,
limits, input recovery, research, evidence and f2-09 integration requirements.

The canonical image installs native programs in `/Programs`. The desktop uses
`PATH=/Programs` and `HOME=/Documents`, opens `/Documents` from its Documents
icon, and keeps the recent list at `/Documents/recent.lst`. The boot-volume
folders display as `Programs`, `Documents`, `System` and `Games`; runtime
`/system` paths continue to resolve through case-insensitive lookup (owner
decision 2026-10-12, f2-35).

Build the existing SDK first, then run:

```sh
python3 -B apps/desktop/build_desktop.py
python3 -B -m unittest discover -s tests/host/desktop -v
```

The production `make desktop` target uses the existing capped build recipe.
The implementer uses the direct Python command under the directive's explicit
sandbox exception. No download, QEMU, systemd or Git command is used by this
recipe. Scratch lives on disk inside the worktree. The manifest records SDK,
source, ELF, portrait and Tango icon store hashes; stale outputs fail image
payload validation. The store (`tango.cis`, pinned in `tango-store.json`) is
built by `convert_icons.py`; see `assets/icons/README.md`.

`protocol.h` is shared with `apps/demo`. **Lead note:** install this header as
`ciuki/desktop.h` through an SDK directive; f2-08 does not allow edits to `sdk/`.
The approved original remains `assets/brand/ciuki-logo.png`. The converter
neither redraws nor restyles it. Generated pixels belong in build output, not
in this source directory. The Ciuki portrait remains a separate project asset;
MIT notices on the code do not relicense it or the GPL ABI header.

## Look and themes (ui-look, 2026-10-12)

The compositor and libdeskui draw through `theme.c`/`theme.h`: one theme
structure holds colours, bevel and title styles, frame radius, shadow size,
title-button size, icon slots (16/22/32) and the font faces. Built-in
themes: **Platinum** (default: platinum greys, blue accent, pinstriped
active title bars, gradient bevels, rounded window tops, soft shadow),
**Ciuki Blue** (slate-blue gradient captions, Ciuki's tan as accent) and
**Classic 98** (flat 3-D greys, navy gradient captions, aliased text). Frame
geometry (`DESK_BORDER` 3, `DESK_TITLE` 24, close/minimise hit boxes) is the
same in every theme, so scripted coordinates never depend on the theme.

Shell and Control Panel API (all in `theme.h`; libdeskui re-exports it):

- `desk_theme` (current, never NULL), `desk_theme_count()`, `desk_theme_at(i)`,
  `desk_theme_named(name)`, `desk_theme_use(name)`; libdeskui clients call
  `du_theme_use(ui, name)` (also marks the window dirty) and `du_theme_name()`.
- Settings file `DESK_SETTINGS_PATH` = `CFG_FILE` = `/system/settings/desktop.cfg`,
  owned by the shell. One format and one writer: `settings.c` (`cfg_parse`,
  `cfg_format`; the shell and the Control Panel). Key `theme` names the
  theme; `desk_settings_theme(text, n, name, size)` reads it with the same
  line rules (comments, trimming, last valid line wins, unknown names ignored).
- `desk_theme_poll(path, now_ns)` re-reads the theme key at most every 2 s
  and returns 1 when the theme changed; the desktop calls it in its loop
  (shell only, never in gate mode) and repaints with `desk_theme_changed()`.
  Every libdeskui client polls it the same way in `du_window_turn` and
  repaints its window. The Control Panel applies a new theme to itself at
  once (`du_theme_use`), saves the file and sends `DESK_SETTINGS` reload,
  after which the shell applies it (`desk_shell_apply`) and repaints.
- Window manager: `desk_shade_box()` (windowshade button left of minimise),
  `desk_collapse()` (toggle; a double-click on the title bar does the same,
  shell only), `desk_outer()` (frame plus drop shadow: the area every frame
  change damages).
- Fonts: `desk_font(id)`, `desk_text_measure(_n)`, `desk_text_fit_chars`,
  `desk_paint_text(_n)` (pen on the baseline). Faces: Inter 12 (UI), Inter 12
  SemiBold, Inter 13 Bold (titles), Inconsolata 12 and 12 Bold (CiukNote,
  6x13 cells), Inter 22 Bold (large), Inter 13 (`DESK_FONT_LABEL`, desktop
  icon labels). U+0020-007E, U+00A0-00FF, U+2022, U+2026.
- libdeskui: `du_text_font`, `du_text_width_font`, `du_label_width`,
  `du_raised`, `du_well`, `du_focus_ring`, `du_shadow`, widget kinds
  `DU_CHECKBOX`, `DU_RADIO` (radios group by `total`) and `DU_TOOLBAR` (band
  behind tool buttons), metrics `DU_MENU_ITEM_H`/`DU_MENU_SEP_H`, icon slots
  `DU_ICON_SMALL/TOOLBAR/LARGE`. The `DU_*` colour names now read the theme.

`convert_fonts.py --prepare` (Pillow with FreeType) rasterizes the faces from
`assets/fonts/upstream` into `ui_fonts.h` (8-bit coverage, ink-box crops,
rounded advances); `--check` (standard library) verifies the pinned source
and header hashes in `ui-fonts.json` and runs in every desktop build.

Design references: Apple, *Mac OS 8 Human Interface Guidelines* (1997),
chapter 5, window guidelines: collapse box, double-click on the title bar,
platinum 3-D controls, title bar height of about 19 pixels
([thig-58](https://discmaster.textfiles.com/file/32774/Dev.CD%20Dec%2098%20RL2.toast/Technical%20Publications/mac/HIGOS8Guide/thig-58.html?html=true),
[thig-81](https://discmaster.textfiles.com/file/31918/Apple%20Developer%20Reference%20Library%20(December%201999)%20(Disk%202).iso/pc/technical%20publications/mac/higos8guide/thig-81.html?html=true));
Microsoft title-bar guidance (caption buttons at the upper right, 16x16
caption icon), [Microsoft Learn: title bar](https://learn.microsoft.com/en-us/windows/apps/design/basics/titlebar-design).
Client surfaces have a fixed size in protocol v2, so windows are not
resizable and "zoom" is the Mac OS 8 windowshade, not a maximise.

Frames and the Ciuki button (ui-polish, 2026-10-12):

- One frame painter, `desk_paint_frame()` in `theme.c`, draws every frame:
  program windows, the DOS window, the built-in About/Run/Shut Down/message
  windows (compositor.c) and libdeskui's modal dialogs (dialog.c), so title
  height (`DESK_FRAME_TITLE` 24, body at +24), outline, corners and buttons
  never diverge. Title-button hit boxes come from `desk_title_button_box()`
  (18x18, 2 px apart, the same in every theme); dialogs show only the close
  button, at the same place (a modal dialog cannot be minimised or rolled up
  on its own). Platinum: pinstripes blended over a soft vertical gradient,
  centred bold title with a light emboss, 15 px bevelled square buttons with
  7x7 glyphs (anti-aliased cross, bar, roll-up/roll-down triangle), a blue
  hover face and a pressed face; inactive frames keep quiet flat buttons and
  no stripes. Ciuki Blue: the same layout on a vertical slate-blue gradient
  with glassy buttons, tan hover outline and the window icon at the left.
  Classic 98: 3-D bevel, navy caption, left title, 16x14 caption buttons.
  Hover follows the pointer (`desk_title_code`), pressed the held left button.
- The Ciuki (Start) button is at the left end of the top bar
  (`DESK_START_X/Y/W/H` = 4, 4, 96, 24; owner request, 2026-10-12), in the
  spirit of the classic Mac OS menu bar, whose pull-down menus drop from
  their titles and whose system menu is a standard menu
  ([Inside Macintosh: Overview, "About Menus"](https://discmaster.textfiles.com/file/32774/Dev.CD%20Dec%2098%20RL2.toast/Technical%20Publications/mac/IMOverview/IMOverview-45.html?html=true);
  platinum menu bar with bevelled edges and anti-aliased corners:
  [Mac OS 8 HIG, "Menu Bar Changes"](https://discmaster.textfiles.com/file/32774/Dev.CD%20Dec%2098%20RL2.toast/Technical%20Publications/mac/HIGOS8Guide/thig-54.html?html=true)):
  the approved portrait (area-averaged, 18 px) and "CiukiOS" on a rounded
  (radius 7, anti-aliased) softly bevelled gradient face around the theme's
  `start` colour, lit on hover, pressed in (`start_open`) while the menu is
  open. The Start menu drops from it at (4, 32), submenus open to the right;
  Win and Ctrl+Esc still open it. The taskbar keeps the task buttons (from
  `DESK_TASK_X` 4, at most 160 px) and the tray.
Reference frames: `DesktopTests.test_look_reference_frames` builds
`tests/host/desktop/look_capture.c` (desktop with a window of every control,
dialogs, Start menu, one pair of 1024x768 frames per theme); with
`CIUKI_LOOK_FRAMES=<dir>` it also copies the PPM frames there for review.
