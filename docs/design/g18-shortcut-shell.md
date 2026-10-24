# G18 shortcut shell behaviour

The G18 shortcut sheet edits the persisted target, arguments, working folder,
shortcut key, initial show command, icon path and icon index. OK saves and closes;
Apply saves without closing; Cancel leaves the source unchanged. Change Icon
focuses the icon path/index controls. Find Target opens the containing folder.
Repair Target explicitly saves the edited absolute target; the shell never guesses
an executable by scanning unrelated directories. Save requests run on the existing
shortcut worker, and errors are reported by its completion path.

This follows Microsoft's [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
contract for the working directory, arguments, show command, icon and hotkey.
[IShellLink::SetHotkey](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinka-sethotkey)
stores a virtual key in the low byte and HOTKEYF modifiers in the high byte.
The sheet captures Ctrl/Alt/Shift combinations; a bare printable key is assigned
Ctrl+Alt so ordinary typing does not launch applications. Desktop and Programs
links participate; repeats do not repeatedly launch a shortcut.

The Programs menu projects installed shortcut source paths beneath
both the active profile's Programs directory and the shared
`/Windows/Start Menu/Programs` directory into nested submenus. Each submenu identifies its
full folder path, so identical folder names at different levels stay separate.
The native Accessories, System Tools and Games menus also include installed
links in the corresponding folders. Other inventory roots remain direct Programs
entries. Discovery remains bounded by the existing 64-link worker snapshot.
The separate Programs window provides access when a menu exceeds screen height.
A displayed menu retains its link snapshot until dismissed, so an installer
cannot reorder a visible item onto another target before the next click.

The Windows 98 Resource Kit, chapter 6, “Configuring the Active Desktop”
(`wrkc06_2.htm`, “Customizing the Programs Menu”), describes the Programs menu
as cascading program groups and applications, with folders alphabetically
arranged initially. The implementation preserves the native catalogue order,
then sorts each installed portion with folders first and alphabetic names.
The local read-only kit was consulted; no Resource Kit content is redistributed.
