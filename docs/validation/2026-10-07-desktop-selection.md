# Desktop and file-window selection

## Research and decision

Apple's user guide describes selecting separate items with Command-click,
selecting a contiguous range with Shift-click, and selecting icons by dragging
from nearby blank space across them. Apple's AppKit guide specifies that the
selection is a set of indexes, that the set should drive multi-item commands,
and that mouse selection can update continuously while dragging. These are the
interaction rules used here. CiukiOS maps the non-contiguous Command gesture to
Ctrl because its desktop keyboard and existing menus expose Ctrl shortcuts.

Sources:

- [Apple Support: Select items on your Mac screen](https://support.apple.com/en-qa/guide/mac-help/mchlp1378/mac)
- [Apple Developer: Enabling Row Selection and User Actions](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/TableView/RowSelection/RowSelection.html)
- [Apple Support: Organize files on the Mac desktop](https://support.apple.com/en-hk/guide/mac-help/mh35951/mac)

The repository already had Ctrl-toggle and Shift-range selection in Files, a
selection count and selected-file copy/cut/delete loops, and desktop icon move
dragging. The missing interaction was a left-button selection marquee in the
desktop and Files content panes; desktop icons also lacked Shift-range
selection. The change keeps existing custom positions in `ICONPOS.DAT`, while
placing default and newly discovered desktop icons from the right edge inward.
The desktop's Arrange and Line Up commands use the same right-edge grid.

The desktop and Files pane both select an item when its visible rectangle
intersects the marquee. Ctrl-drag adds to the current selection. A plain
background click clears selection, and a plain marquee replaces it. Existing
Ctrl-click and Shift-click remain available. File actions that already operate
on `items[i].sel` continue to apply to the complete set; opening and renaming
remain single-item operations.

## Desktop icon labels

The [W3C CSS Text Module Level 3](https://www.w3.org/TR/css-text-3/) defines
soft wrapping at allowed break opportunities and allows otherwise-unbreakable
text to wrap at character boundaries when needed to prevent overflow. The
desktop applies the same practical rule in its fixed-width icon cells: prefer
spaces, then break a long filename at a complete UTF-8 sequence. It renders the
whole stored long filename rather than copying an arbitrary 64-byte prefix or
fitting it to two lines. The vertical grid pitch grows to fit the tallest
label, and drawing, selection/focus rectangles, damage, hit testing, marquee
intersection, and right-edge auto-placement all use the expanded bounds. The
existing 8.3 alias remains only the explicit key used by `ICONPOS.DAT`; label
text and file operations keep the full LFN.

The Files large-icon view uses the same production wrapper. Its fixed 92-pixel
column keeps the full name centered across as many lines as needed, while the
row pitch grows to contain the label. The hit rectangle, keyboard focus,
selection background, scrolling, and marquee bounds use that expanded row, so
adjacent names do not overlap and the full label remains selectable.

## Validation

Validation:

- `git diff --check -- src/apps/desktop.c src/apps/files.c` passed.
- The OpenWatcom app compiler passed on each changed module using the same
  warning-as-error options as `scripts/build_apps.sh` (`-q -2 -ms -s -zl -os
  -d0 -wx -we -zq -zm -i=src/apps`), writing objects under `/tmp`.
- OpenWatcom warning-as-error compiles passed for the updated Desktop and Files
  modules. `python3 scripts/test_icon_labels.py` compiles the shared production
  helper and checks a 100-byte filename, word boundaries, complete UTF-8
  sequences, and cell geometry at 800, 1024, and 1280 pixel screen widths. It
  passed without a QEMU run. This host check validates wrapping and cell bounds;
  it does not claim a captured QEMU rendering of a 100-byte label at each mode.
- The canonical full image build passed. The sequential capped QEMU gate
  `build/full/t23-vbe-fix/selection-final/report.json` passes actual held-button
  marquee selection on the desktop and in Files, Ctrl-toggle, multi-file copy,
  multi-item recursive copy/move/recycle through four directory levels, and
  independent FAT16 content checks of all five nested source/recycled files.
  Default icons occupy the right-hand columns (x=974 and 890 at 1024 pixels).
  This earlier full-image snapshot SHA-256 was
  `f8e17a92afba4a5897efbd57b82bbbb2475031423878cc0bc987db048c9b2cca`.
  The report marks `physical_tested: false`; no physical-runtime qualification
  is inferred from this emulator test.
- The later full HDD QEMU run at
  `build/full/t23-vbe-fix/selection-properties/report.json` passes desktop and
  Files marquee selection, Ctrl-toggle, multi-file copy, and recursive
  copy/move/recycle of a three-level tree plus a sibling file. Its independent
  FAT check confirms all five nested file hashes and that the source tree was
  unchanged. The report marks `physical_tested: false`. This report does not
  contain an image hash; the matching Properties run records canonical full
  image SHA-256
  `6514765d51bc4d839788f1ec749065204b98ed254cbca2d3b2097188112e5959`.
- The full-image capture
  `build/full/t23-vbe-fix/properties-1280-damage/1280-virtio/first/default-Ciuk1-Fill.png`
  shows the built-in desktop's right-column labels fully visible at 1280×800,
  including Control Panel, DOS Prompt, Floppy, USB drive, and CD-ROM. This is a
  separate visual check from the selection report. The 100-byte filename test
  above remains a host-side helper/geometry check; it does not claim that a
  100-byte label was rendered in this QEMU capture.
# Right-edge glyph visibility

The first integrated 1280×800 screenshot still omitted the final `el` of
`Control Panel` and the `t` of `DOS Prompt`. `ui_text` rejected a whole glyph
unless 24 pixels remained at the screen edge; that margin belongs to the direct
planar writer, whose shifted glyph touches three bytes. The compositor already
clips each actual pixel to its owned band. Composed text now enters that clipped
path directly, retaining the planar footprint guard for direct planar writes.
This distinguishes [glyph ink from layout advance](https://www.w3.org/TR/css-overflow/#ink).
Desktop image/audio file icons also use the same approved Tango mime icons as
Files. Integrated runtime pixel checks remain required after this correction.
