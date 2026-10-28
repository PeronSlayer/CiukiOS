# Users, logon and passwords (G32)

Security-model supersession: the owner's real-user decision is specified in
[user-security-contract.md](user-security-contract.md) and its
[kernel enforcement/ABI half](user-security-kernel.md), reconciled under
[sec-contract](../directives/sec-contract.md) and the binding
[lead answers](../directives/sec-lead-decisions.md). Implementation and runtime
evidence remain pending; the single [implementation plan](user-security-contract.md#implementation-plan)
covers both halves. The profile-only model, optional logon and Cancel-to-default
below describe historical G32 behavior, not acceptance criteria for the new
kernel-enforced model. Earlier host evidence remains evidence for G32 only.
The replacement uses SYSTEM uid/gid 0, administrator/Standard/Guest credentials,
explicit G32 enrollment into new uids, mapped numeric private profiles and
shared /Documents. It removes anonymous cancellation, provides a trusted lock
and session teardown, and distinguishes passworded Safe Mode uid 500 from an
unset ready-image recovery account. Standard users may create creator-owned
root files/folders; Guest remains read-only on shared/removable media. Password
recovery without a usable administrator uses explicit Setup CD repair, never
an automatic empty-password account.
Round-1 amendment: blank CiukiUser is allowed only as the sole enabled interactive
account; round 2 requires a password first before enabling another account or
blank rename/demotion (no atomic replacement), and retired autologon never returns on reboot. The contracts now
freeze explicit role-bound launch/secure-console/service operations, batch owner
publication and seed recovery, while retaining DOS C: private FAT16/D: read-only
system/E: removable. See the [interface delta](user-security-contract.md#delta-for-sec-k01-and-sec-user-01)
for the implementation directives already in progress. Round 2 also freezes
correlated authentication, own-session desktop lock, monotonic secure-console
epochs, best-effort cancellation with durable caller STATUS, OWNER-resolved
staging/checkpoint windows, and authorized queries for mapped profile roots.
These remain future acceptance requirements, not new G32 test evidence.

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

## Integration merge (2026-10-24)

The integration branch already supplies the G22 Multimedia page at index 15.
Keep that index and append Users at 16 and Passwords at 17 (18 pages total),
with matching page-name/label arrays and the shared applet catalogue. G12
input widgets keep their existing 1500-series IDs; G32 uses 1600-series IDs.
There are no new wire messages, syscalls or SDK ABI values in this merge.

Rechecked the [Microsoft logoff contract](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off)
and [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
before resolving the overlap. Preserve G3's veto-aware logoff path. Desktop
drops use the selected profile's Desktop (or the default Desktop when unset)
and retain G8's move/copy/link effects, so shortcut creation and profile
personalisation coexist. Window-model evidence retains all earlier scenarios
and appends printing and the G32 gate.

## g32b

Research checked 2026-10-26 against the owner's read-only Resource Kit:
`wrkc07_3.htm` (profile USER.DAT and personal folders), `wrkc09_4.htm`
(Windows and screen saver passwords; Cancel bypasses logon),
`wrkc18_2.htm` (local logon) and `wrkc30_4.htm` (Advanced's standby password
option uses the screen saver password). These describe Windows 98 profiles,
not NT access control. Keep the existing local password gate and shared FAT.

Primary online references:

- [Microsoft: Handling Screen Savers](https://learn.microsoft.com/en-us/windows/win32/lwef/screen-saver-library)
  and [ScreenSaverProc](https://learn.microsoft.com/en-us/windows/win32/api/scrnsave/nf-scrnsave-screensaverproc):
  idle/SC_SCREENSAVE activation and input termination. Track full-screen
  Preview separately; arm the desktop-owned password gate before waking a
  protected display. Preview never arms it. Resume uses the same gate,
  independently of the saver option.
- [Microsoft: GetUserNameA](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamea):
  the returned size includes the terminator and an undersized buffer fails.
  Return the selected local name, rather than the registered owner.
- [Microsoft: CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl)
  and [SHGetSpecialFolderPathA](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetspecialfolderpatha):
  personal folders belong to the selected profile; retrieval and creation
  are distinct. Resolve through that profile's Shell Folders keys.

Implementation decisions: recover users.lock under an existing service-owned
writer lease (the INI transaction lease), with atomic owner publication in a
nonempty directory as the restart-safe fence (review round 1 below). Record boot
and process identities, keep live contention busy and retain temp/fsync/replace
saves. Host filesystem fixtures use a permanent flock guard, which likewise releases on process exit.
Profile publication uses a private application operation over the existing
authenticated registry channel. The desktop is its only writer. Do not switch
while announced Win32 clients survive; G3 must end them first. Separate USER
hive slots under each numeric profile root retain the existing crash-safe
registry format, rather than claiming binary Windows USER.DAT compatibility.
Verification and outstanding lead qualification are recorded below on completion.

The completed wiring also refreshes the desktop's Users policy asynchronously
when Control Panel publishes changed settings. Display's password checkbox
uses the same durable policy, preserves the other flags, and its Change button
opens Passwords. Unlock authentication never republishes profile selection
(policy changes to separate profiles apply at the next logon).

Boot identity comes from the durably committed, nonsaturated recovery boot counter
(binding failure is logged and never reports VALID); owner liveness
comes from the process snapshot. Missing boot identity or an unreadable snapshot
fails conservatively without removing a live lock. The native lock adapter is
host-tested with injected service/recovery/process calls; the Linux filesystem
adapter additionally exercises a real exited owner and a held flock guard.
The [Linux flock reference](https://man7.org/linux/man-pages/man2/flock.2.html)
documents independent opens and release at last close; the guard file itself is
never unlinked. No new syscall, public ABI record/constant or SDK header is added.

Profile hives use `USER.CRG.0`/`USER.CRG.1` beneath the existing numeric profile
root and seed from the default hive on first selection. Each switch flushes the
old hive and loads/validates the new hive before publishing identity. A new
profile alone inherits and rebinds default folder values; existing profiles
preserve redirections and deleted values/keys. Failed commits and corrupt
existing slots preserve the current session; they never silently reseed an existing profile. Announced
Win32 clients and open registry handles prevent changing identity. G3 logoff
ends the clients; their exit flushes the selected hive. Service restarts inherit
the desktop's selected name/root under the publication/snapshot mutex. PROGMAN
follows the selected Programs folder;
common CSIDLs retain system paths. Folder service failures are returned, rather
than resolving a personal folder into another profile.

`GetUserNameA/W` now uses the returned name length, including its terminator;
the no-logon case returns ERROR_NOT_LOGGED_ON (1245). Microsoft's
[preserved GetUserName example, Q139625](https://jeffpar.github.io/kbarchive/kb/139/Q139625/)
confirms use of the Windows logon identity. The precise no-logon error is a
compatibility inference: the cited Microsoft API page does not document Cancel
on Windows 98. The lead should confirm it against original Windows 98; this
implementation does not invent a user named Ciuki or Default.

Window-model rows 108 and 109 add protected saver/SC_SCREENSAVE, Preview,
default/disabled policy, failed DPMS wake, Cancel/wrong password, concealed
scanout, suspended DPMS drawing grants, locally refused standby, device recovery
failure after sleep and firmware resume counters. Native and Win32 account,
registry and shell harnesses exercise the production paths. Exact commands and
results, including unavailable leak checks under ptrace, are in
`build/codex/g32b-report.md`. Guest and hardware qualification remain lead-owned.

### g32b review round 1

The review fixes retain the Windows 98 profile model above. Rechecked the
Microsoft screen saver, ScreenSaverProc, GetUserName and CSIDL references:
conceal scanout before DPMS suspends display grants; wake from that concealed
frame before drawing the password gate. Standby keeps the gate after calling
the firmware-facing operation even on errors, since device recovery may fail
after sleep. Locally refused entry may discard only the gate it prepared.

Account acquisition must publish its owner atomically and survive service-lease
loss. FAT has no hard links in the native SDK. Publish a nonempty lock directory
by rename, containing a complete owner record with a unique acquisition name.
Rename cannot replace a nonempty directory ([Linux man-pages, rename(2)](https://man7.org/linux/man-pages/man2/rename.2.html));
`proc/posixpath.c:rename_inner` enforces the same rule. Recovery removes only the
observed owner's uniquely named entry, then the empty directory, so an old
recoverer cannot remove a replacement owner's record. Incomplete legacy records
fail busy because their boot/owner cannot be established safely. The existing
service lease remains a fast contention guard, rather than the safety fence.

Use the amendment's durable-counter alternative for boot identity: accept a
bootstat binding only after increment write and flush succeed, refuse saturation,
and report bind failure at mount. No public ABI extension is needed. Profile
publication acknowledgement and cache update share a mutex with restart's
snapshot and spawn. Rebind inherited shell folder defaults only when a profile
hive is first created; persisted redirections and deletions remain authoritative.
The kernel user-security contract and the SYSTEM services now supersede the profile-only rules
above (sec-k01..sec-k07, sec-user-01..sec-user-05 on the security integration branch); this
section keeps G32's recorded history. Runtime cases: `tests/suites/sec-users.json` and acc-t23
items 10-18 (sec-user-06).
