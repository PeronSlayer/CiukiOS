# G18 shell links and special folders

Expected behavior is based on Microsoft's [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links), [IShellLink::SetHotkey](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinka-sethotkey), [IShellLink::GetHotkey](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinka-gethotkey), and [CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl) documentation.

Shell links preserve launch metadata, including their shortcut key, through
IPersistFile Save and Load. The shortcut key's low byte is a virtual key; its
high byte holds Shift, Control, Alt and extended-key flags. Zero removes the
shortcut key. Unknown flag bits, a modifier without a virtual key, and bits
outside the 16-bit key encoding are invalid in CiukiOS. Failed loads preserve
the existing object, and failed setters preserve the previous key.

CIUKLNK3 keeps its existing 40-byte header and five counted UTF-8 fields. The
formerly reserved word at byte 36 now stores this validated 16-bit hotkey;
the upper 16 bits remain zero. Existing links with a zero reserved word keep
working. Both the desktop and IShellLink writer use the same encoding. This
is CiukiOS's existing link format, not Microsoft's MS-SHLLINK file format.

CSIDL_RECENT (8) identifies the directory of recent-document shortcuts;
CSIDL_SENDTO (9) identifies the directory of user Send To items. CiukiOS's
single-user Windows 98 layout maps them to `C:\WINDOWS\Recent` and
`C:\WINDOWS\SendTo`, respectively. A/W folder APIs return the same paths;
the existing create flag creates those directories. New registry hives seed
matching Recent and SendTo values in Explorer Shell Folders and User Shell
Folders. Existing persistent hives retain their user changes. Returning a
Recent folder does not itself insert documents or implement SHAddToRecentDocs.

Large temporary encoding and file buffers in the touched IShellLink methods
use checked heap allocations so every touched SDK C file compiles with the
4 KiB frame limit.
