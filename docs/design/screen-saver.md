# G10 screen saver and monitor power

Expected behaviour and implementation contract, researched before implementation.

> **Superseded in part by user security** (sec-user-06 reconciliation, 2026-10-28). A password-protected saver locks through
> LOGON's trusted gate when it is dismissed, never through a desktop overlay; a password-less account
> keeps only the concealment ([lock, saver and resume](user-security-contract.md#43-lock-saver-and-resume), sec-user-04).
> Runtime: acc-accounts item 16 and su-09-lock (`[desktop] saver start/wake`, `[logon] session S locked
> by the screen saver`, `[security] console enter ... reason=saver`).

Windows 98 Resource Kit, chapter 30 (`wrkc30_4.htm`, local read-only reference),
identifies keyboard, mouse and joystick activity as use, and describes the Power
Management page's inactivity interval for turning off the monitor. Appendix D
(`wrkappd_3.htm`) lists optional savers. Chapter 9 (`wrkc09_4.htm`) makes the
screen-saver password conditional on password protection being selected.
Password protection remains G32, as the G10 inventory explicitly specifies.

Microsoft's [screen saver library](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library),
[system commands](https://learn.microsoft.com/en-us/windows/win32/menurc/wm-syscommand)
and [SystemParametersInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa)
define idle activation, input dismissal, preview, timeout in seconds, enabled
and running state. Mask the low four system-command bits. SC_MONITORPOWER uses
-1 for on, 1 for low power and 2 for off. Modern secure-saver actions are not
claimed as Windows 98 functionality. Unsupported secure setters must fail.

The upstream [Xorg VBE implementation (source mirror)](https://fossies.org/linux/misc/xorg-server-26.0.99.902.tar.xz/xorg-server-26.0.99.902/hw/xfree86/int10/vbe.c)
(`VBEDPMSSet`) uses INT 10h AX=4F10h, BL=1 and BH=0/1/2/4
(on/standby/suspend/off), checking AX=004Fh. CiukiOS uses the existing bounded
BIOS VM, probes capabilities (BL=0), then sets only supported power states.
The framebuffer transaction stops acceleration and releases VGA ownership
before firmware runs; it remains suspended while the monitor is off and
commits the unchanged framebuffer layout on wake. No mode-set or firmware
access is allowed concurrently with a DOS or game display lease.

G10 adds operations 2 (shared screen policy) and 3 (DPMS) to syscall 74,
whose previous highest operation was 1. It adds no syscall or desktop message
number. The shared fixed-width record is checked by the ABI oracle. GET/SET
SPI actions use the Win32 numbers, CONFIG and STATUS are desktop-only.
Applications can request the configured saver or monitor power through system
commands; the desktop consumes the most recent request at its next service
turn. Single-session policy is system-wide, rather than process-local.
SPIF_UPDATEINIFILE requests persistence through the desktop settings worker.

Blank, Ciuki logo and Starfield use a full-screen compositor overlay. The logo
uses the existing pinned deterministic conversion of assets/brand/ciuki-logo.png;
no new artwork is generated. Preview runs full-screen until input. The wait
control uses minutes and persists seconds. Zero disables the respective timer.
The kernel reports inhibition for the independent DOS full-screen transaction
as well as delegated game display ownership; every attempted saver start queries
that state again. DOS display leases, game display leases, full-screen windows, session shutdown,
and active drags suppress automatic activation and reset the idle epoch. Input
that dismisses a saver is consumed, preventing accidental clicks on applications.
Key releases/resync still repair the desktop input state. Wake restores the
monitor and invalidates the entire scene; animation damages full frames at a
bounded cadence. Saver configuration has no additional per-saver options.

Host evidence and limitations are recorded in build/codex/g10-report.md;
firmware and physical panel behaviour require lead hardware qualification.

## fx-28 Display Properties refinement

The [screenshot-review contract](screenshot-review-fx-28.md) records the primary
references. The tab now has a CRT miniature (100 ms updates only for animated
choices, sharing the full-screen saver geometry), a drop-down beginning with (None), Settings, Preview, password controls
and a bounded editable 1–1440 minute spinner. (None) disables activation without
losing the saved saver kind. Password protected and Change are disabled with the
reason that G32 session lock is not installed. Monitor Settings launches `control --panel=power` through the existing
DESK_LAUNCH request; OPEN_POWER means Shut Down. Apply and Cancel retain their existing transaction.
