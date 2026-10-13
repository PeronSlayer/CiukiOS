# F2-53 taskbar tray layout

Research checked on 2026-10-13 before implementation:

- [Microsoft notification area documentation](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)
  associates input and tooltips with an icon's rectangle and recommends placing
  its popup near that icon. This is a UI reference, not a Win32 API dependency.
- [Freedesktop icon theme specification](https://specifications.freedesktop.org/icon-theme/latest/)
  defines size-specific icon lookup. Retain the existing Tango 0.8.90 store's
  native 16 px images, without rescaling or changing their artwork.

Repository comparison: `apps/desktop/shell.c` reserves a fixed 152 px tray,
positions network, volume and media from its left edge, and anchors all popups
to the tray's right edge. The media slot remains reserved when no media exists.
The thin sunken frame and vertically centred clock are already present.

Directive decision: measure the displayed clock text, then pack volume, network
and visible removable media from right to left with 4 px gaps and 6 px inner
margins. Keep shared rectangles beside taskbar geometry in `compositor.c` so
toolkit programs can link the compositor without the shell renderer. Share
those rectangles across rendering, hit tests, popup placement,
damage and task-button sizing. Keep the existing keyboard handlers. Media
visibility and clock-width transitions (including settings reload) must invalidate the taskbar because
they also move its left edge and resize task buttons. Retain native icon
canvases on a common vertical baseline and the toolkit's disabled treatment.

Host checks and before/after PNGs are recorded in
`build/codex/f2-53-report.md`. QEMU and integration validation belong to the lead.

Host result on 2026-10-14: `nice -n 19 python3 -B -m unittest discover -s
tests/host/desktop -t . -v` completed 85 tests, OK with five standard skips
requiring native desktop payloads absent from this worktree. Tray geometry,
native Tango/disabled pixels, incremental/full redraw agreement, compositor
goldens and existing application case replays passed. Reference PNG comparisons
for all three themes differ only inside the taskbar. `git diff --check` passed.
