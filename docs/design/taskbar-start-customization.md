# Taskbar and Start menu customization (G20)

## Windows 98 behavior

Taskbar Properties exposes Always on Top, Auto hide, Show small icons in Start
Menu, and Show Clock. Auto hide leaves a narrow edge that reveals the bar when
the pointer reaches it; Always on Top controls whether ordinary maximized
windows can cover the taskbar. The Start Menu Programs tab supports adding and
removing menu entries, reordering entries by dragging, and clearing Documents
and the other personal history lists. Recent Documents is capped at 15 entries.
Quick Launch is a toolbar of shortcuts, including Show Desktop.

The Windows 98 Resource Kit describes Start menu customization as adding,
removing, and reordering entries, including dragging an item to the Start
button; its Folder Options dialog controls the global folder display style and
advanced file-view settings. CiukiOS keeps its established Start menu and
folder view models and applies these choices to the existing shell/settings
files rather than introducing a registry or ABI contract for native apps.

## CiukiOS implementation contract

Persist taskbar display and Start-menu choices in `/system/settings/desktop.cfg`
using the existing versioned parser/writer. Defaults preserve the current
always-visible, clock-visible shell. The taskbar context-menu Properties entry
opens the Taskbar & Start Menu settings page. Quick Launch contains Show
Desktop and routes to the existing `desk_show_desktop` action. Clearing
Documents clears the desktop list and the shared `/Documents/recent.lst` list
used by applications. Folder Options remains shared with Files so hidden-file
and extension preferences are not per-window.

Every state change that alters painted shell pixels or geometry must damage
both the old and new regions and be represented in the fx-15 redraw model.
Host tests cover setting parse/format, taskbar hit-testing and redraws, recent
history clearing, and persistence/reload behavior.

## Sources and implementation decision

- Microsoft, *Windows 98 Resource Kit*, Chapter 6, “Configuring the Start and
  Programs Menus”, “Configuring the Taskbar and Toolbars”, and “Customizing
  Folders”, locally available at `build/rskt/wrkc06_2.htm` (sections found by
  the headings above). It specifies add/remove/reorder, drag-to-Start, Quick
  Launch, and global Folder Options.
- Microsoft KB Q199161, “Unable to See or Use the Taskbar” (Windows 95/98),
  <https://helparchive.huntertur.net/document/106670>. It describes Always on
  Top, Auto hide, small Start-menu icons, and Show Clock.
- Microsoft Learn, “The Taskbar”,
  <https://learn.microsoft.com/en-us/windows/win32/shell/taskbar>. It describes
  the clock, taskbar context menu, auto-hide, always-on-top, and work-area
  relationship.
- Microsoft Learn, “Managing the File System”,
  <https://learn.microsoft.com/en-us/windows/win32/shell/manage>. It specifies
  that the shell's recent-document list is presented by the Start menu and can
  be cleared through the shell API.

The compact CiukiOS display uses a separate top shell bar and bottom taskbar;
the same behavior is adapted to these existing regions. The G20 implementation
in this change delivers the persisted Show Clock and Quick Launch visibility
choices, the Show Desktop button, and a Taskbar & Start Menu page reachable
from both taskbar Properties and Settings. Always on Top, Auto hide, small
Start-menu icons, Start-menu pin editing and reorder, the app recent-list merge,
and a Settings shortcut to Folder Options remain open gaps; see the G20 row in
`os-feature-inventory.md`.
