# Desktop display preference and startup recovery

Research for `docs/directives/fx-dispmode.md`, 2026-10-27.

Lead note 1 identifies the preceding session as Safe Mode. Safe Mode uses
temporary boot geometry: it must neither apply nor replace the normal saved
mode, including when that mode is absent from the reduced inventory. The
owner also requires preserving resolution-specific desktop icon positions.
Suppress their position/arrangement writes in Safe Mode while still allowing
unrelated preferences to save. Detect Safe Mode from the boot flags as well as
the existing `--safe` argument; the optional recovery adapter is not required
for this protection. Startup recovery below applies only to normal boots.

- The local Windows 98 Resource Kit, chapter 24, `wrkc24_4.htm`, describes
  recovery through the standard VGA driver and `DisplayFallBack=1`.
  Microsoft's [Q188481, Windows 98 Display.txt](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/188/481.HTM)
  describes resetting unusable settings to defaults through Safe Mode, then
  choosing working settings. It does not establish automatic recovery in every
  normal Windows 98 boot. CiukiOS implements the directive's equivalent using
  its already working boot framebuffer: retain it, reset the saved preference
  to its mode, and explain recovery in a Display Settings message.
- [ChangeDisplaySettings](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsa)
  distinguishes temporary `CDS_FULLSCREEN` changes from registry updates.
  Desktop mode notifications are therefore observations, not permission to
  save a preference. Display Properties already persists its confirmed mode;
  desktop reload must adopt that durable value. Win32 settings remain in their
  own registry; leases, DOS entry/return and watchdog rollbacks must not save
  the native desktop preference.
- X.Org's [Savage manual, UseBIOS](https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man4/savage.4.html)
  explains why mobile panel timing uses the BIOS by default. The desktop's
  nondelegated mode-set path already uses BIOS VBE; it must retain that policy.
  [VBE 2.0, functions 4F01/4F02/4F03](https://www.phatcode.net/res/221/files/vbe20.pdf)
  supplies mode information, changes the mode and reads it back. Failure at
  preparation, firmware execution, decoding, framebuffer ownership or commit
  must be distinguishable in the diagnostic record, without a native-panel
  fallback or a new panic path.

The private serial capture was examined in place. Boot 4 contains a successful
mode 280 change, a long interval without a completed frame, and then mode 274.
The latter is consistent with the existing ten-second frame watchdog restoring
the previous framebuffer. The old frame completion code would then save 274.
The capture alone cannot identify which earlier session originally saved a
preference, or which desktop input the user delivered before the missing restart.
Host replay and remaining hardware questions are recorded in
`build/codex/fx-dispmode-report.md`.

The implementation leaves `biosvm.c` and panel admission unchanged. The original
EIO record cannot distinguish firmware failure from framebuffer preparation or
ownership failure. Added failure-stage records preserve that distinction for
qualification. Likewise, 640x480 Restart reaches the production power worker and
the reboot syscall boundary in host replay, including Safe Mode; the original
missing request remains unlocalized. These are open physical-evidence items,
not demonstrated hardware fixes. The power/V86 sanitizer runs are blocked at
LeakSanitizer exit by the sandbox's ptrace restriction; leak detection remains
enabled.
