# G20 Taskbar and Start menu customization

The binding behavior comes from Microsoft's Windows 98 Resource Kit chapter 6,
“Configuring the Start and Programs Menus” and “Configuring the Taskbar and
Toolbars” (`wrkc06_2.htm` in the owner's read-only extracted kit), and chapter 3
(`wrkc03_2.htm`). Start items are folder-backed shortcuts: dropping on Start
adds a shortcut, dragging within a menu reorders it, and its context menu can
open, delete, rename or explore it. The Programs property tab offers Add,
Remove, Advanced and Clear Documents. Clearing removes the shared applications'
recent list as well as the shell's list; Documents shows at most 15 unique paths.
Quick Launch has its own folder, accepts dropped shortcuts and launches with
one click; Show Desktop uses the existing reversible minimize-all operation.
Address accepts paths/URLs; Links, Desktop and custom toolbars enumerate their
backing folders. New Toolbar selects an existing folder.

Microsoft's [Taskbar](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar)
documents the pre-Windows-7 Always on Top option, Auto hide, optional toolbars
and a work area excluding the reserved taskbar. The
[application desktop toolbar](https://learn.microsoft.com/en-us/windows/win32/shell/application-desktop-toolbars)
contract describes docking and resizing on all four screen edges. The
[filesystem shell documentation](https://learn.microsoft.com/en-us/windows/win32/shell/manage)
describes the shared recent documents list. These sources are researched before
implementation; Windows 98 behavior takes precedence over later Windows UI.

Implementation: retain CiukiOS's approved portrait Start button and system tray
in the existing top bar. The window taskbar docks at bottom, top (below the
system bar), left or right; dragging empty space moves it, its inner border
resizes it. Settings persist edge, thickness, visibility and toolbar folders.
Always on Top controls taskbar composition and hit testing against windows.
Auto hide leaves a two-pixel activation strip and reveals on edge motion,
hiding after leaving unless a menu or drag owns it. Maximizing/arranging uses
a shared work rectangle; Auto hide reserves only the activation strip.

Shell input/rendering remains free of filesystem I/O: it requests scans and
mutations serviced by desktop.c. Snapshots are bounded and shortcuts use the
existing CIUKLNK3 format; no public protocol changes or new kernel interfaces.
Folder Options opens Files' existing property sheet directly.

Defaults are Always on Top on, Auto hide off, normal (32 px) Start icons,
clock and Quick Launch on, bottom edge and 32-pixel thickness. The small
Start icon setting uses the existing 16-pixel artwork. The portrait, Tango
family, top Start/tray arrangement and English copy remain the approved ones.
Always on Top off lets maximized windows cover the taskbar; selecting the bar
raises it until a window takes focus. Auto hide reserves two pixels, reveals
on edge motion and hides after 500 ms away, held open by menus, Address editing,
a drag or taskbar keyboard focus. A border drag is clamped to half the screen
(and 512 pixels), with a 32-pixel minimum. Esc or input resynchronization cancels
a geometry drag. Normal maximization and arrangement use the shared work area.

The shell publishes folder snapshots through a dedicated worker. Its initial
native menu population creates Accessories, System Tools, Games, Developer and
StartUp shortcuts once; a marker prevents deleted defaults returning at the
next start. A read-only or failed population keeps the compiled Programs menu
available. Installed CIUKLNK3 shortcuts and folder submenus share the same
scanner. Folder-local `.order` files persist drag ordering; unreadable or stale
entries in that order file are ignored. Shortcuts are re-read before launch.
The existing native/DOS/Win32 launcher receives their target, arguments and show
mode. Games retain their existing properties entry. Menus scroll when needed,
including keyboard selection of the first and last items.

Quick Launch uses `/Windows/Application Data/Microsoft/Internet Explorer/Quick Launch`.
Hovering over Start for 500 ms opens it during an external drag; folder rows
open their submenus and accept shortcuts in that folder. Drops on Start or a
shortcut toolbar create shortcuts without moving the source;
a folder dropped onto an empty taskbar creates a toolbar. The Address field is
editable in place (Enter opens, Esc leaves, Ctrl+A clears). Links reads
`/Windows/Favorites/Links`; Desktop reads `/Documents/Desktop`; Favorites reads
`/Windows/Favorites`, including Internet Shortcut `.url` files. Folder toolbar
buttons show their contents and expose overflow menus. An overall chevron makes
all enabled toolbars accessible on narrow screens. Right-clicking a toolbar
name offers Close and Open Folder. New Toolbar validates an existing folder
before storing it. Toolbar visibility and the four custom folder paths persist
with the taskbar settings.

Start item right-click offers Open, Delete, Rename, Explore and Properties;
Programs items have the same operations and drag ordering. The Programs property
tab has an Add wizard (target, arguments, Programs subfolder, title), a selectable
Remove list with confirmation, Advanced opening the Start Menu folder, and
Clear Documents. A successful clear removes the shared application list and
both in-memory and persisted shell history; failed I/O retains the shell list.
A reload serial prevents an older worker result restoring pre-clear preferences.
Settings > Folder Options launches Files with a deferred property-sheet request,
after its initial listing finishes. The Find entry uses G7's existing shell hook;
Log Off and the other user/session features remain G32's responsibility.

Resource bounds are explicit: 32 folder snapshots, 256 entries, 24 children per
folder, eight menu levels and four custom toolbars. More... Open Folder keeps
larger folders reachable. One drop may create at most 24 shortcuts. File paths
are bounded at 255 bytes; oversized paths are refused rather than truncated.
The native bootstrap starts the file worker; shutdown waits for pending writes,
and cancelling power preparation restarts the worker with retry backoff.
Unrelated settings reloads preserve an open menu.

No ABI, public SDK header, syscall/message allocation or vendored code changes
are required. fx-15 continuation rows are appended as 43–47, after part 1's 42.
