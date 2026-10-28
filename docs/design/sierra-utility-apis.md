# Sierra utility API implementation notes

The fx-hlutils directive requires reusable API behaviour for the Sierra
utilities; resolving an import to a success stub is insufficient.

Primary references:

- [Microsoft BeginPath](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-beginpath):
  opening a bracket discards the previous path; drawing inside it records geometry.
- [Microsoft PathToRegion](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-pathtoregion):
  a closed path becomes a region in device coordinates and is consumed.
- [Wine 10.0 path implementation](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/win32u/path.c):
  reference for bracket state, figure closure, flattening and replay.
- [Microsoft SetCommState](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcommstate):
  configure the existing port without flushing its queues; reject identical
  XON/XOFF characters and invalid UART framing.
- [Microsoft joyGetPosEx](https://learn.microsoft.com/en-us/windows/win32/api/joystickapi/nf-joystickapi-joygetposex):
  validate size, ID and requested fields; distinguish an unplugged device.
- [Wine oledlg build](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/oledlg/Makefile.in)
  and [Wine wininet build](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/wininet/Makefile.in):
  wininet imports ws2_32, with security libraries delay-loaded. Reuse must retain
  the normal binding to CiukiOS Winsock, and must pass the existing import and
  H0/H1 qualification gates before staging.

Repository comparison: GDI already has WMF recording, WMF/EMF playback and
validated byte buffers. Extend those facilities. The native ABI already exposes
COM line/status/modem controls and USB/game-port joystick queries. Reuse those
controls, preserving native ownership checks. Keep path geometry per DC in
device coordinates; mapping changes after recording must not move the path.
The scalar renderer must continue to pass its i686 FPU/SIMD audit.

The test wrapper named by the directive calls systemd-run. The task's explicit
hard prohibition overrides that wrapper requirement: host commands run serially
with nice -n 19. No image, QEMU or hardware evidence is produced here.

WinINet import closure additionally requires loader references, computer-name
queries and IPv4 resolver wrappers. References:
[Microsoft runtime linking](https://learn.microsoft.com/en-us/windows/win32/dlls/run-time-dynamic-linking),
[Microsoft getnameinfo](https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getnameinfo),
[Microsoft IdnToAscii](https://learn.microsoft.com/en-us/windows/win32/api/winnls/nf-winnls-idntoascii),
and the pinned Wine `dlls/wininet/utility.c`, `http.c` and `dialogs.c`.
The native network stack is IPv4: address-formatting and wide-name wrappers
reuse that resolver and reject unsupported families. ASCII DNS labels are
validated and normalized; IDN names requiring Unicode Nameprep or ACE decoding
are refused with ERROR_INVALID_NAME until a Unicode IDNA provider is supplied.
Unimplemented TLS, SSPI authentication and persistent password storage return
explicit WinINet/Win32 errors before unavailable providers can be invoked.

The new Win32 joystick system action is read-only and requires a live SYSTEM
or interactive-session credential. It only enumerates joystick devices and
queries joystick caps/state, rejects keyboard-only devices, and clears any
keyboard state on a composite device. Calibration and route mutations are
excluded. Existing native records carry all data; no second wire layout is
introduced. COM controls retain the original file object, lease generation
and device credential checks. A new native COM_CLEAR_ERRORS operation returns
the queue/status snapshot and acknowledges sticky UART errors under the port
lock, without clearing queued data.

UART reconfiguration also follows the FIFO reset-bit semantics in the
[TI TL16C2550 datasheet](https://www.ti.com/lit/ds/symlink/tl16c2550.pdf)
(FIFO control register, bits 1 and 2). Opening/resetting a port may clear the
FIFOs; COM_SET_LINE preserves them. The host UART model checks that line
configuration does not issue FIFO-reset writes. The Windows 98 Resource Kit
local text `wrkc12_4.htm` identifies both USB and MIDI/game-port joystick
connections; both feed the existing native joystick controls here.

Compatibility boundaries for review: the scalar GDI renderer uses bounded
curve subdivision and rasterized round stroke outlines. Existing extended-pen
cap/join limitations still apply. The font provider exposes raster glyphs, so
text inside a path fails with ERROR_NOT_SUPPORTED rather than recording a
rectangle or silently dropping the text. Color-adjustment state is validated,
saved and restored; the existing HALFTONE renderer does not yet apply its
gamma/color transform. Resetting printer driver settings is also unsupported.
These are remaining rendering gaps, not evidence of complete GDI compatibility.

The UART facade supports immediate polling and finite constant read/write
timeouts. Flow control, 1.5 stop bits, finite inter-byte or multiplier timeouts,
infinite waits and overlapped COM handles are explicitly refused with
ERROR_NOT_SUPPORTED. SetupComm queue sizes are advisory. USB joystick results
are normalized native HID axes; JOY_RETURNRAWDATA is unsupported for USB, while
the analog path can return raw values. Legacy capture messages contain the
first four buttons; extended polling exposes the native button mask.

The new Wine DLLs have complete static import closure, but remain unqualified
until H1 executes successfully. No private-CD import census or application
execution is claimed by this implementation record.
