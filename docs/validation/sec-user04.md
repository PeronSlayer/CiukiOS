# sec-user-04 User Accounts, saver and resume

Scope: sec-wave4 second round, contract row 7 (H12, RUS, RWM and the existing
saver/power/control harnesses), plus the items earlier steps left to it: the
password page's notice to LOGON, the password-protected saver and Stand
by/resume through LOGON, an administrator ending another user's locked
session, G32 upgrade enrollment (contract amendment), Add/Remove Programs'
per-user shortcuts, the XP shell-folder registry values, a per-user My
Documents and `joystick_settings_session_end` on the session teardown path.
Host evidence only; no QEMU, image or hardware result is claimed. Binding
specification: [user-security-contract.md](../design/user-security-contract.md)
4.3, 4.4, 6.3, 7.1, 8.2, the implementation plan row 7 and the amendment
"G32 enrollment source and account controls (sec-user-04 amendment)";
[user-security-kernel.md](../design/user-security-kernel.md) "Sessions and
logoff" and the A2-3 table; [sec-user03](../../../CiukiOS-wt-sec-user03/docs/validation/sec-user03.md)
for LOGON/LOGON_UI.

## Primary sources and resulting decisions

| Source | Finding | CiukiOS decision |
| --- | --- | --- |
| Microsoft, [How to determine your user account type in Windows](https://support.microsoft.com/en-US/Accounts-Billing/manage/how-to-determine-your-user-account-type-in-windows) (Windows XP steps) | XP's User Accounts in Control Panel lists each account with its type beside the picture; Standard accounts are for everyday use, administrator accounts give the most control. | One **User Accounts** applet replaces Users and Passwords; the list shows name and type; "passwords" stays a launch alias that opens the caller's own password task. |
| Microsoft, [Windows XP Fast User Switching: Design Guide](https://learn.microsoft.com/en-us/previous-versions/ms997634(v=msdn.10)) | XP accounts are NT profiles; users are created in Control Panel; per-user data in My Documents (CSIDL_PERSONAL), all-users Start Menu entries in the common profile; Users have read-only HKLM; applications "degrade gracefully": disable the operation in the first place and show "You must be an administrator to perform this operation". | Controls a caller may not use stay visible but disabled, with "You must be an administrator to change this setting." (contract 7.1 wording); the SYSTEM account service re-checks every change at apply time; My Documents is the profile's Documents; HKLM gets the Common values only. |
| Microsoft, [User Accounts with Fast User Switching and Remote Desktop](https://learn.microsoft.com/en-us/windows/win32/shell/fastuserswitching) | FUS keeps several users logged on at once (Terminal Services sessions). | Not adopted (contract 4.4): one interactive session; no enabled Switch User. Changing users is Log Off, or an administrator's log-off of a locked session. |
| Microsoft, [LockWorkStation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-lockworkstation) | Same result as Ctrl+Alt+Del then Lock; to unlock, the user must log in. | Lock, the protected saver and password-on-resume are one trusted lock: SESSION_EVENT/SESSION_LOCK to LOGON, which owns SECURE_ENTER/LEAVE. |
| Microsoft, [WlxScreenSaverNotify](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxscreensavernotify) | Winlogon is told before a screen saver starts; pSecure says whether the workstation is locked when the saver is dismissed; before the call Winlogon makes its own desktop current and the workstation locked. | A password-protected saver's dismissal enters the trusted gate (SECURE_ENTER SAVER) **before** the desktop is restored; a failed entry keeps the saver (fail closed). The saver itself stays the desktop's (no trusted saver desktop in CiukiOS). |
| Microsoft, [ScreenSaverIsSecure](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-2000-server/cc959646(v=technet.10)) (HKCU\Control Panel\Desktop) | The "Password protected" choice of the Screen Saver tab is a per-user value. | Per-user `secure_saver` line in the user's own settings ("On resume, password protect", XP wording); a preference never authorizes: LOGON locks only an owner who has a password. |
| Microsoft, [Prompt for password on resume](https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/no-subgroup-settings-prompt-for-password-on-resume) | The resume prompt is enabled by default in every Windows desktop edition (Vista and later; XP had the same Power Options checkbox, "Prompt for password when computer resumes from standby"). | Per-user `secure_resume` line, on by default once a password exists (contract 4.3); armed **before** Stand by: LOGON enters SECURE_ENTER(RESUME) and the desktop waits (at most 3 s) for the gate before the call-73 STANDBY. |
| Microsoft, [WlxWkstaLockedSAS](https://learn.microsoft.com/en-us/windows/win32/api/winwlx/nf-winwlx-wlxwkstalockedsas) | While locked, the GINA may keep the lock, unlock, log off, or force a logoff (WLX_SAS_ACTION_FORCE_LOGOFF), the workstation staying locked until the logoff completes. | "Log off this user (administrator)" on the locked screen: an administrator's typed name and password end the locked session forced (after a warning that unsaved work is lost), without creating a session; the administrator then logs on afresh. A Standard user or wrong password changes nothing. |
| Microsoft, [CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl) | CSIDL_COMMON_STARTMENU/STARTUP/APPDATA/DOCUMENTS/PROGRAMS/DESKTOPDIRECTORY are the All Users folders; CSIDL_PERSONAL/STARTMENU/PROGRAMS/STARTUP/DESKTOPDIRECTORY the user's. | HKLM Shell Folders and User Shell Folders gain "Common Start Menu", "Common Startup", "Common AppData", "Common Documents" (the locations `sdk/win32/shell/folders.c` answers); HKCU keeps sec-user-05's per-user values; Add/Remove Programs cleans both scopes' shortcuts. |

## Repository comparison (before this step)

- Control Panel: two G32 pages (Users, Passwords) on `apps/desktop/users_io.c`
  with "Show logon at startup" and "Separate ... settings" options; no role
  awareness on any page; the saver checkbox wrote G32's machine USERS_LOCK.
- Desktop of a LOGON session: protected saver dismissal and resume called
  SESSION_LOCK (a blank owner got the set-password guidance and Control Panel
  on every saver dismissal); resume locked only after the firmware returned;
  the session's Desktop and Start Menu were the common folders.
- LOGON: AUTH LIST ENOENT showed "Logon is not available" on a G32 upgrade
  (no interface listed the protected source); no administrator override of
  a locked session; notice PASSWORD existed but nothing sent it.
- Kernel: `joystick_settings_session_end` (sec-user-05) had no caller.

## Implementation

- **User Accounts** (`apps/control/accounts.inc`, `accounts_client.{h,c}`):
  the production client of ACCOUNTS (service 2): QUERY_SELF for the caller's
  role, LIST, then RESERVE -> management (312-byte body, client operation
  id) -> lost reply settled through STATUS (never repeated, never inferred
  as rollback) -> LIST. Tasks: create (Standard or administrator), name,
  password (own: old/new/confirm; another account: reset), picture (default
  picture; no picture set is installed yet), type, turn on/off, delete.
  Standard users and Guest see only their own account; administrator tasks
  are disabled with the reason. Results map the service's errors (EPERM
  role/last-administrator, EBUSY account in use, EACCES old password,
  EINVAL names/passwords, EAGAIN changed list). Interim images without the
  account service keep the G32 pages.
- **Passwords alias and notice**: `--panel=passwords` opens the own-password
  task; a changed own password sends DESK_SETTINGS_PASSWORD_SET to the
  desktop, which sends SESSION_EVENT notice PASSWORD to LOGON.
- **Saver/resume**: desktop `saver_wake` (LOGON session) wakes DPMS first (the
  pre-DPMS black frame conceals scanout), then SAVER notice, then restores;
  Stand by arms the RESUME gate before call 73; an unarmed firmware resume
  locks at once. Per-user preferences `secure_saver`, `secure_resume`
  (`apps/desktop/session_prefs.h`), shown in Display > Screen Saver and
  Power > Advanced. Power's standby/disk timers and button/lid choices are
  disabled for Standard users and Guest (machine policy, contract 7.1).
- **LOGON/LOGON_UI**: SESSION_EVENT SAVER/STANDBY, UI_STATE OVERRIDE/
  OVERRIDE_CANCEL, the enrollment mode and ENROLL_SOURCE relay (amendment).
- **ACCOUNT**: AUTH ENROLL_SOURCE=8 (`enrollment.c`, `protocol.c`,
  `service.c`, `auth.c`). **Kernel**: route/limit for AUTH 8 and LOGON 10
  (`channel.c`, `security.c`); `joystick_settings_session_end` in
  `secure_console_poll`'s drain of a closing session.
- **Shell folders and documents**: LOGON-session Desktop/Start Menu are the
  profile's own (`logon_session.c`; sec-user-05's merges add All Users);
  My Documents defaults in Files Send To, Terminal, Recorder, Clipboard
  Viewer, Registry Editor export, System Information/Monitor, Serial Viewer
  and Backup use the profile home; HKLM Common values (`apps/w32srv/defaults.c`).

## Limits kept

- Standby stays unavailable where the provider/firmware/drivers are not
  qualified (`power_source_standby_reason`); DPMS limits unchanged.
- No account picture set is installed (the Pictures store under
  `/System/Security/Pictures` is empty on images); the picture task sets the
  default picture (the approved Ciuki portrait) only.
- LOGON_UI offers no change of an already staged enrollment plan.
- Fast User Switching remains out of scope (contract 4.4).
