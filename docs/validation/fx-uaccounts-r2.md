# Control Panel completion adoption, round 2

The directed behavior is to finish an admitted account change and queue its
session notification before normal window close. Microsoft's
[window closing documentation](https://learn.microsoft.com/en-us/windows/win32/learnwin32/closing-the-window)
allows an application to defer destruction when processing a close request.
It does not specify XP User Accounts' asynchronous implementation; that ordering
is the directive's requirement, not a claim established by this documentation.

Microsoft's [dialog documentation](https://learn.microsoft.com/en-us/windows/win32/dlgbox/about-dialog-boxes)
and [SetCapture documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setcapture)
describe modal ownership and capture lifetime. In CiukiOS a page rebuild destroys
those UI states. Consequently the common completion adopter must defer while
either is active, including calls reached through heartbeat or pending close.

Repository comparison: accounts adoption reads mutable account_uid, cp_close
allows an active accounts request, game_poll_result joins menu_job, and cp_tick
bypasses idle's guard. Preserve the submitted UID, defer normal close until
account adoption, remove the menu join, and guard cp_jobs_poll itself. Exercise
production initialization, window FIFO/flush, desktop dispatch and LOGON state
with kernel-boundary host fakes. No ABI or password policy change is required.

Microsoft's [Fast User Switching](https://learn.microsoft.com/en-us/windows/win32/shell/fast-user-switching)
explicitly describes XP's separation of account profiles and application/session
state. The harness therefore preserves the kernel caller/session identity while
changing the applet's selected account. This does not add fast user switching.
The historical KB279783 URL now redirects to a Windows 10/11 support page; it
was not used to establish XP behavior or asynchronous close ordering.
