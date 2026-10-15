# f2-75 — Control Panel and the desktop context menu's Properties/Display entries: put them in order (owner request 2026-10-15)

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f2-75` from `main`. Build the desktop before reporting.

## Request

The owner finds the Control Panel and the desktop right-click "Properties" / "Display" entries confusing. Audit first (append to `docs/design/desktop-interaction-audit.md`: every Control Panel applet, every entry of the desktop context menu, what each opens, duplicates and dead ends), then:

1. **One Display Properties dialog** with tabs: Background (wallpaper picture, placement, colour), Appearance (theme, colours, fonts), Screen Saver if present, Settings (resolution, colour depth, the Savage engine state as read-only information); reachable from the desktop context menu as "Properties" (the only display-related entry there) and from the Control Panel as "Display"; Apply/OK/Cancel behave the same everywhere (Apply previews, Cancel reverts); the same dialog code is used by both entry points.
2. **Control Panel: categorization is the point** (owner's words: everything must be well categorized). Build a category tree first and record it in the audit document for the lead: every setting of the system appears in exactly one category and one applet, applets are grouped and named by what the user wants to do, nothing is scattered across unrelated panels, and the same categories are used by the Start menu's Settings submenu and by the tray context menus. Then the Control Panel organized by that tree with consistent icons and names (Display, Sounds and Audio, Keyboard, Mouse, Date and Time, Network, Power, Storage and removable devices, System: version, memory, logs, Hardware check), one applet per item, no duplicated or orphan panels; the window remembers its size; keyboard navigation; the same layout language as the rest of the desktop (f2-64/f2-67).
3. Desktop context menu: Arrange icons, Line up, Refresh, New (folder, text file), Paste, separator, Properties — in that order; remove anything that duplicates the Start menu.
4. Goldens re-recorded and PNGs under `build/host/f2-75/` for every applet and the dialog's tabs.

## Acceptance

Desktop suite and gate green; the owner judges on the T23.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; plain prose, no names of other operating systems. Report `build/codex/f2-75-report.md`.
