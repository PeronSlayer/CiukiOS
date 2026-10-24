# G18 shortcut launch and persistence

The Microsoft [Shell Links contract](https://learn.microsoft.com/en-us/windows/win32/shell/links)
records target, arguments, working directory, show command, icon location and
hotkey independently. Activating the link uses its saved initial show command.
[STARTUPINFO](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfoa)
with `STARTF_USESHOWWINDOW` supplies the first ShowWindow state. The existing
native shell helper passed only target/arguments/cwd to a native spawn and lost
this state. G18 passes a validated private `CIUKI_SHELL_SHOW=1`, `3`, or `7`
environment handoff into the initial PE startup. The personality converts it
into normal process parameters only when no explicit Win32 startup record is
present. Subsequent CreateProcess startup data takes precedence, even if the
record does not set STARTF_USESHOWWINDOW. USER applies
those parameters to the first ShowWindow call and SW_SHOWDEFAULT.

The shortcut sheet edits the cached record and submits persistence to the
existing sleeping shortcut worker. The input thread performs no disk writes.
A successful same-directory temporary-file rename replaces the shortcut;
failures before replacement retain the prior file and produce a visible error.
The existing durable writer also checks the replacement file's fsync result.
The worker validates
all fields before writing and rescans after completion. Repair is explicit:
the user replaces Target and Start in in the sheet, preserving arguments,
hotkey, icon and Run mode. Automatic object-ID tracking after a move is not
claimed; no unrelated executable is selected by name.

[Microsoft's resource format](https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats)
stores icon DIB XOR pixels followed by the monochrome AND mask.
The shell decodes bounded classic ICO images with 1/4/8/24/32-bit BI_RGB DIBs,
selects the nearest suitable size, and publishes 16/32 pixel ARGB snapshots.
[About Icons](https://learn.microsoft.com/en-us/windows/win32/menurc/about-icons)
describes choosing the image matching display size and depth. Unsupported PNG
or compressed DIB entries retain the Tango executable fallback. The exact Ciuki
identity assets are unchanged. Cached pixels are copied to a main-thread
snapshot before rendering; no decoder or file I/O runs in paint.
