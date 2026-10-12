# Ciuki desktop (f2-08)

An ordinary SDK-built ring-3 compositor. The supervisor owns launch, exclusive
grants and the desktop's separate process group. See
[the desktop guide](../../docs/desktop.md) for wire layout, launch arguments,
limits, input recovery, research, evidence and f2-09 integration requirements.

Build the existing SDK first, then run:

```sh
python3 -B apps/desktop/build_desktop.py
python3 -B -m unittest discover -s tests/host/desktop -v
```

The production `make desktop` target uses the existing capped build recipe.
The implementer uses the direct Python command under the directive's explicit
sandbox exception. No download, QEMU, systemd or Git command is used by this
recipe. Scratch lives on disk inside the worktree. The manifest records SDK,
source, ELF and portrait hashes; stale outputs fail image payload validation.

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
- Settings file `DESK_SETTINGS_PATH` = `/system/settings/desktop.cfg`, owned
  by the shell: `key=value` lines, `#` comments, at most 16 keys. Key
  `theme` names the theme. `desk_settings_load/get/set/save` read and write
  it (`save` creates `/system/settings`). Unknown keys are preserved.
- `desk_theme_poll(path, now_ns)` re-reads the theme key at most every 2 s
  (stat mtime/size) and returns 1 when the theme changed; the desktop calls
  it in its loop (shell only, never in gate mode) and repaints with
  `desk_theme_changed()`. Every libdeskui client polls it the same way in
  `du_window_turn` and repaints its window.
- Window manager: `desk_shade_box()` (windowshade button left of minimise),
  `desk_collapse()` (toggle; a double-click on the title bar does the same,
  shell only), `desk_outer()` (frame plus drop shadow: the area every frame
  change damages).
- Fonts: `desk_font(id)`, `desk_text_measure(_n)`, `desk_text_fit_chars`,
  `desk_paint_text(_n)` (pen on the baseline). Faces: Inter 12 (UI), Inter 12
  SemiBold, Inter 13 Bold (titles), Inconsolata 12 and 12 Bold (CiukNote,
  6x13 cells), Inter 22 Bold (large). U+0020-007E, U+00A0-00FF, U+2022, U+2026.
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
Reference frames: `DesktopTests.test_look_reference_frames` builds
`tests/host/desktop/look_capture.c` (desktop with a window of every control,
dialogs, Start menu, one pair of 1024x768 frames per theme); with
`CIUKI_LOOK_FRAMES=<dir>` it also copies the PPM frames there for review.
