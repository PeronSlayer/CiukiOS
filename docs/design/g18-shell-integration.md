# G18 Win32 shell integration

The binding scope is inventory item G18. Implementation extends the existing
desktop v3 transport and shell worker; it introduces no kernel service.

Expected behaviour, researched before implementation:

- [Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicona)
  and [NOTIFYICONDATA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataa):
  ADD, MODIFY and DELETE identify an icon by its window and numeric ID. MODIFY
  changes only flagged fields. The shell owns a pixel copy, delivers mouse
  messages through the selected callback (wParam=ID, lParam=mouse message), and
  removes icons when their owner window dies. ANSI and Unicode text are decoded
  separately. Unknown operations and malformed sizes fail.
- [The Taskbar](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar):
  notification icons have tooltips and application-handled menus. Balloon
  notifications are a later Shell extension explicitly required by G18, not a
  claim about the base Windows 98 API. A bounded FIFO defers program balloons
  while another balloon is visible; entries are removed with their icon.
- [FlashWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-flashwindow):
  TRUE toggles the drawn caption/task button once, FALSE restores its normal
  appearance, return is its previously drawn active state; focus never changes.
  The application supplies a timer if it wants repeated flashes.
- [WM_DROPFILES](https://learn.microsoft.com/en-us/windows/win32/shell/wm-dropfiles):
  DragAcceptFiles enables a target. A successful desktop drop posts WM_DROPFILES
  with a process-owned HDROP and zero lParam. File names and client point survive
  until DragFinish. Native data-object paths are translated to Windows drives
  and encoded as UTF-16 DROPFILES; malformed lists are rejected atomically.
- [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links):
  shortcut launch retains target, arguments, working directory, show mode, icon
  location and hotkey. Properties allow explicit correction of a missing target;
  an unrelated executable must never be guessed as a replacement.

The local Windows 98 Resource Kit `wrkc06_2.htm`, “Configuring the Start and
Programs Menus” (read from the owner's read-only extraction), describes
alphabetical cascading program groups backed by folders. G18 retains those
folders instead of flattening the first eight links. Native built-in menu
categories remain available. Startup execution belongs to G21.

Wire additions follow DESK_V3_PRESENT (79), G5 clipboard (80/81) and G4 limits
(82): TRAY_ICON=83, TRAY_BALLOON=84, TRAY_EVENT=85, FLASH=86. Existing
ACCEPT_FILES=47 and DROP_FILES=48 gain v3
envelopes. Requests remain bound to the authenticated peer, window ID and
generation. No pointer/Win32 handle is interpreted by the shell. Icon pixels
travel through a read-only mapped 16×16 surface and are copied before release.

Each v3 DROP_FILES owns a surface containing a length word and an independent
serialized copy of the shell-owned path list. The producer unmaps its writable
mapping before enqueueing. Existing kernel channel attachment rules
(`proc/channel.c:channel_send`, `proc/surface.c:surface_attachment`) accept only
surface grants and cap the receiver at PROT_READ; regular file descriptors cannot
cross this boundary. USER validates SURFACE_INFO, maps read-only, copies the
counted bytes, unmaps and closes the transferred descriptor before constructing
HDROP. The sender closes on successful send or queue teardown. A subsequent drag
cannot replace an earlier queued drop's bytes. No kernel policy or ABI changes.
