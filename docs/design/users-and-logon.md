# Users, logon and passwords (G32)

Written 2026-10-23, after reading the G32 inventory and the batch headers.

## Reference behaviour and implementation decision

The owner's Windows 98 Resource Kit, chapter 7, **How User Profiles Work**
(`wrkc07_3.htm`), describes a local profile selected at logon, a default
profile when logon is cancelled, and personalised Desktop, Start Menu,
My Documents, Favorites and settings. Chapter 18, **Logging on to Windows
98** (`wrkc18_2.htm`), distinguishes local Windows/Family Logon (a list of
users) from network authentication. Chapter 9, **Passwords**
(`wrkc09_4.htm`), specifies old password, new password and confirmation;
chapter 23, Table 23.1 (`wrkc23_2.htm`), lists Passwords and Users applets.
These pages were read from the lead's read-only `build/rskt` extraction.

The [Microsoft logoff documentation](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off)
and [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession)
require an application veto to cancel logoff. Reuse G3's existing query/end
protocol, including ENDSESSION_LOGOFF, before returning to logon; never
switch profiles while a save prompt is outstanding. Do not power off.

The [screen saver documentation](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
describes idle activation, full-screen concealment and input dismissal.
G32 provides a desktop-owned opaque password gate and a lock entry point;
G10 must call it on dismissal of a password-protected saver. Its timer,
saver selection and suppression policy are G10's responsibility.

This tree has no account service, saver implementation or editable native
Start Menu. Desktop content is `/Documents/Desktop`, settings are a single
`/system/settings/desktop.cfg`, and G3 logoff currently returns to that same
desktop. G32 adds an internal app service without new syscalls, SDK records
or wire messages. Default single-user use preserves those paths. Named
profiles use stable numeric directories under `/Windows/Profiles`, separate
Desktop, Documents, Start Menu/Programs, Favorites and desktop settings.
User names are case-insensitive ASCII letters/digits/underscore/hyphen,
1–31 bytes; passwords are case-sensitive printable ASCII, at most 63 bytes.
User deletion retains files and never recycles a profile identifier.

Local profiles personalise a FAT-based shared machine; they do not provide
filesystem access control or network authentication. Cancel at logon uses
the default profile, as in Windows 98. Cancel at a password lock keeps the
session locked. No NT access-token or NTVDM semantics are implied.

Password verifiers use the existing SHA-256 implementation and independently
implemented PBKDF2-HMAC-SHA-256 ([RFC 8018, sections 4 and 5.2](https://www.rfc-editor.org/rfc/rfc8018.html)).
A fresh 16-byte salt comes from `getentropy`; failure aborts the operation.
The bounded format uses 10,000 iterations (recorded format version 1),
constant-time verifier comparison and a SHA-256 corruption checksum.
Passwords are never stored in settings or shown in text widgets. This is a
retro-machine local logon facility, not a modern credential vault.
Disk I/O and password derivation run on workers; UI callbacks only submit
copied requests and take completed results. Saves use a same-directory
temporary, fsync and replacement. Concurrent applet writers are rejected
with an exclusive lock file; stale locks after a crash require recovery.

## Integration boundaries

The G10 and G20 implementations are absent at this source state. G32 exposes
the lock hook and profile Start Menu path for their integration. Full Win32
HKCU/user-name/CSIDL rebinding, network roaming and remote-administration
passwords need their service owners; they are not represented as delivered.
The inventory and `build/codex/g32-report.md` record the exact tested subset.
