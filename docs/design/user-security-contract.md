# User security: accounts, logon, Setup and application compatibility

Directive: [sec-model](../directives/sec-model.md). Author: Codex.
Written 2026-10-26, after checking the workstation clock.
Source baseline: `17fd29b2a4d09aa78ba9b1109b11e0f25b0e8b85`.
Status: design proposal for lead cross-review; implementation and runtime
qualification are pending. The owner decisions and shared lead decisions below
are binding. Proposed policy decisions become binding after that review; unresolved
questions are collected in the final section.

## 1. Decision and scope

CiukiOS will authenticate local users and enforce their rights in the kernel.
A profile selects personal files and preferences; a credential authorizes access.
Selecting a name, changing an environment variable or starting a particular binary
MUST NOT grant another user's rights.

The owner's user/security decision supersedes the profile-only portion of
[G32](users-and-logon.md) and the Windows 9x security assumptions in
[execution-abi.md](execution-abi.md), [vfs-storage-contract.md](vfs-storage-contract.md),
[unified-dos-contract.md](unified-dos-contract.md),
[win32-personality-contract.md](win32-personality-contract.md),
[registry-contract.md](registry-contract.md) and
[cd-setup-plan.md](cd-setup-plan.md). It does not replace their storage, V86,
device, scheduling, Setup transaction or Windows 98 compatibility designs.

The parallel kernel half is `docs/design/user-security-kernel.md`. It owns
credential records, syscall/IPC layouts and numbering, path resolution and owner
metadata, handle checks, process isolation, privileged operations and trusted
display/input transitions. That document was not present in this worktree while
this proposal was drafted. This half specifies consumer requirements, not a
second kernel ABI. A disagreement blocks the affected implementation directive
until the lead reconciles both halves; this text MUST NOT silently supply missing
kernel mechanics.

### 1.1 Binding shared decisions

| Subject | Shared requirement |
| --- | --- |
| Identities | 32-bit user ids. 0 = SYSTEM (kernel, boot services, the logon/account service); 500 = CiukiAdministrator; 501 = Guest; 1000 and up = ordinary accounts (CiukiUser or the Setup user is 1000). Ids are never reused. |
| Groups | NT well-known RIDs as numbers: 544 Administrators, 545 Users, 546 Guests. Account types shown to the user: Administrator (544+545) and Standard/Limited user (545); Guest (546). |
| Process credentials | Each process carries an immutable credential record {uid, group set, session id, flags}; children inherit it; only SYSTEM (the logon service) can create a process with a different credential. No setuid binaries. |
| Administrator execution | Administrators run with full administrator rights (Windows XP model, no UAC split token). A Run as administrator credential prompt is deferred; installers can be launched after logging on as an administrator. |
| FAT32 enforcement | FAT32 has no ACLs. Kernel path-prefix rules and protected profile-owner metadata on CiukiOS-managed volumes enforce the policy; XP default NTFS permissions inform the policy, not the FAT on-disk format. |
| Account store | SYSTEM-only, salted PBKDF2-HMAC-SHA-256 verifiers; recoverable locking. |
| Boundary | Physical access, disk encryption and network authentication are outside this step. Existing per-binary authority mechanisms must map into this model. |

CiukiUser is an ordinary administrator account, not SYSTEM and not RID 500.
CiukiAdministrator's special purpose is recovery and its built-in identity; it
does not bypass SYSTEM-only account-store or credential-creation rules.

The directed Guest group set stays 546 only. XP's interactive Guest can also
receive Users membership (R3); copying that extra group would contradict the
shared lead decision. Guest rights here are therefore an explicit policy rather
than an assertion that every XP Guest token is identical.

### 1.2 Threat model and exclusions

Protect one local user's private profile and machine configuration against other
unprivileged native, Win32 and DOS programs, including direct syscall callers,
forged service messages and path aliases. Protect password verifiers from every
interactive account, including administrators: management requests go through the
SYSTEM account service. Administrators are trusted to administer the machine and
can install privileged system updates; this is not containment of a hostile
administrator or a hostile kernel/driver.

Protect logon/unlock input from the existing user's applications. The trusted
SYSTEM logon interface needs exclusive input and presentation, including over DOS
and Win32 full-screen leases. The existing desktop-owned G32 overlay alone does
not establish this boundary. Kernel transitions required for that boundary belong
to the kernel half. Until qualified, SS-3 MUST remain partial.

Offline FAT32 access can read or alter the entire disk. No confidentiality or
integrity against another booted OS, disk removal, firmware compromise or physical
reset is claimed. Network clients continue to work as applications, but there are
no domain accounts, remote logon, anonymous network logon, network password
providers, EFS, discretionary NTFS ACL editor or full NT security subsystem here.
Untrusted code running as the same user can access that user's files; no separate
application sandbox is promised.

## 2. Research and repository comparison

Sources below were inspected for this contract. Historical Microsoft-authored
book chapters hosted by a mirror are identified explicitly. Modern API pages
provide interface semantics, not evidence of historical installation defaults.
No Windows installation was booted or measured for this document.

| Ref | Primary source and finding | CiukiOS consequence |
| --- | --- | --- |
| R1 | Microsoft [well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids): account RIDs 500/501, built-in group RIDs 544/545/546, SYSTEM SID and distinct SID authorities. | Numeric native IDs are not themselves complete Windows SIDs; translate consistently. |
| R2 | Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens) and [privileges](https://learn.microsoft.com/en-us/windows/win32/secauthz/privileges): identity/groups travel with execution; system-task privileges differ from object rights. | Authenticate once, inherit immutable credentials, check both operation rights and object scope. No per-binary route around account rights. |
| R3 | Microsoft Windows XP Professional Resource Kit, [User Accounts and Security Groups](https://flylib.com/books/en/2.922.1.135/1/) (Microsoft-authored text, mirror): Administrator/Guest accounts, Limited users, protection of system/program files and machine registry; Power Users are a separate compatibility role. | Use the three directed account types. Do not invent a Power Users tier or treat Administrator as SYSTEM. |
| R4 | Microsoft Press XP Professional Training Kit, [Understanding User Accounts](https://flylib.com/books/en/2.732.1.17/1/) (mirror): Guest disabled by default; built-in Administrator omitted from Welcome when another account exists; XP Home restricts its use to Safe Mode. | Keep visibility, normal enablement and recovery eligibility distinct. The owner's disabled-by-default Administrator is an explicit CiukiOS policy, not an XP factory-default claim. |
| R5 | Microsoft [MS02-064](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064): XP root permissions include administrator/SYSTEM control, Users read/execute and directory-creation exceptions; upgrades may retain older permissive settings. | The simpler managed-volume policy below deliberately disallows arbitrary root creation by Users. Do not copy permissive legacy roots during migration. |
| R6 | Microsoft [Gaming with Least-Privileged User Accounts](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/gaming-with-least-privileged-user-accounts): XP NTFS protects Windows/Program Files; personal/shared data have writable locations; FAT32 does not supply that protection; limited-user HKLM access is read-only. | Apply an explicit kernel path policy and service-enforced registry policy. Compatibility must use permitted data locations. |
| R7 | Microsoft [registry key security/access rights](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights) and [predefined keys](https://learn.microsoft.com/en-us/windows/win32/sysinfo/predefined-keys): requested key access is checked; HKCU identifies the process user's HKU branch. | Scope handles to caller identity, bind HKCU at session creation, and protect backing files independently of Win32 APIs. |
| R8 | XP Resource Kit, [Authentication Policy Options](https://flylib.com/books/en/2.922.1.130/1/) (Microsoft-authored text, mirror): local account lockout is disabled by default (threshold 0); duration has no effect without a threshold. Password minimum defaults to zero; complexity is not enabled by default. | Choose no persistent lockout, with bounded retry throttling; stricter nonempty passwords below are CiukiOS policy. Current Windows lockout defaults are not substituted for XP. |
| R9 | Microsoft [Time](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/cc770579(v=ws.11)) explicitly applies to XP and requires administrative credentials to change time; [InitiateSystemShutdownEx](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw) distinguishes local/remote shutdown rights. Compaq's original [Installing Windows XP on Compaq Commercial Products, p.16](https://ftp.zx.net.nz/pub/mirror/ftp.compaq.com/pub/supportinformation/papers/15u1-1001a-wwen_rev1_us.pdf) (vendor paper mirrored from its FTP, indexed passage readable, full PDF fetch unavailable) documents Limited users unable to format Zip media and the administrative policy override. Microsoft [removable-media policy](https://learn.microsoft.com/fr-fr/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/devices-allowed-to-format-and-eject-removable-media) is a later reference. | Separate clock writes, local power requests, media data access, eject and raw formatting. Our format restriction follows the XP behavior; ordinary safe removal remains the explicit CiukiOS choice below. Do not present later client defaults as XP evidence. |
| R10 | Microsoft [logoff](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off) and [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession). | Preserve G3 veto/save handling before destroying a session. |
| R11 | Microsoft [GetUserName](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamea), [OpenProcessToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken), [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation), [CheckTokenMembership](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-checktokenmembership), [IsUserAnAdmin](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-isuseranadmin). | Implement real read-only identity/token queries and documented buffer/error conventions, not unconditional success or a fixed administrator answer. |
| R12 | Microsoft [AllocateAndInitializeSid](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-allocateandinitializesid), [LookupAccountSid](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lookupaccountsida) and [CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl). | Supply bounded SID translation and profile-derived shell folders. |
| R13 | [RFC 8018 sections 4, 5.2](https://www.rfc-editor.org/rfc/rfc8018.html): PBKDF2 parameters include salt, PRF and iteration count. | Keep G32's algorithm and parameterized records; do not describe a fixed iteration count as a present-day security recommendation. |
| R14 | The Open Group [getpwnam/getpwnam_r](https://pubs.opengroup.org/onlinepubs/009695099/functions/getpwnam.html), [pwd.h](https://pubs.opengroup.org/onlinepubs/009604499/basedefs/pwd.h.html), [setuid](https://pubs.opengroup.org/onlinepubs/009604599/functions/setuid.html); upstream Linux man-pages [getuid/geteuid](https://www.man7.org/linux/man-pages/man2/getuid.2.html) corroborates the identity-query distinction. | Native libc queries use kernel IDs; public account lookup never exposes verifiers; credential-changing calls cannot elevate. Current Open Group Issue 8 pages returned HTTP 403, so the readable older specification/search text is identified rather than claimed as a full Issue 8 review. |
| R15 | Owner's read-only Windows 98 Resource Kit: `wrkc07_3.htm:10-53` and `wrkc18_2.htm:10-23,65-75`, under `/home/peronslayer/Desktop/CiukiOS/build/rskt`. | Confirms G32's default profile without logon and local-versus-network distinction. Use only as the superseded profile baseline; do not redistribute its text. |

### 2.1 Actual baseline and required changes

| Inspected implementation | Observed behavior | Required change |
| --- | --- | --- |
| `apps/desktop/users.h`, `users.c:104-154`, `users_io.c:56-102` | 16 G32 records, IDs from 1, `CIUKIUSR` v1, fixed 10,000 iterations, shared `/system/settings/users.dat`; exclusive `users.lock` has no crash-owner recovery. | SYSTEM service owns v2 storage, authorization, migration and recoverable serialization. |
| `apps/desktop/logon.inc:31-73`, `profile.h` | Desktop-owned lock; Cancel at logon selects default; profile paths follow caller environment. | Trusted logon/lock and mandatory authenticated identity; environment remains a convenience only. |
| `src/kernel/include/ciuki/process.h:55-80`, `proc/supervisor.c:176-206` | DOS/program/network/DHCP/time/recovery/Setup capabilities are separately delegated by trusted parent plus binary path. | Map to account roles/operation scope in section 7; executable names cannot confer roles. |
| `src/kernel/core/power.c:417-424`, `proc/setup_clock.c:12-15` | Power uses display/input ownership or Setup capability; Setup clock uses Setup mode plus capability. | Separate session requests, machine policy changes and boot-only Setup privilege. |
| `apps/w32srv/registry.c:213-220,396-398`, `storage.c:113-153`; `sdk/win32/advapi32/security.c` | HKCU is `.DEFAULT`, username is `Ciuki`, common USER slots; security structures exist but NT token functions are outside the old security model. | Per-identity hives, authenticated token queries and server access checks; retain data-structure helpers. |
| `apps/control/configuration.c:30-40`, `apps/setup/pages.c:352,631-632`, `inf.c:254`, `scripts/build_image.py:844-865` | Setup stores identity labels; builder makes registry defaults; neither is account provisioning. | Transactional account bootstrap and separate ready-image policy. |
| `docs/design/screen-saver.md`, `power-policy.md:200-202`, inventory SS/PW/DS rows | Saver/resume password UI is incomplete or disabled; existing G32 evidence is not secure-session evidence. | Connect both to the same trusted lock service and replace completion criteria. |

These are bounded source inspections, not claims that the new behavior was tested.

## 3. Accounts and credential storage

### 3.1 Account lifecycle

| Account | Creation and groups | Enablement and logon |
| --- | --- | --- |
| SYSTEM, uid 0 | Internal kernel/boot/logon identity, never an interactive account. | Never listed, never password authenticated, never selected by name. |
| CiukiAdministrator, uid 500 | Reserved built-in recovery administrator, groups 544+545. Cannot be deleted, renamed or converted to Standard. | Disabled and hidden in normal boot by default. Setup sets a password. Safe Mode eligibility requires Q1; it never removes password verification. Explicit administrator enablement makes it available at normal logon. |
| Guest, uid 501 | Reserved, groups 546 only; cannot be deleted or made administrator. | Disabled by default. Proposed enablement requires an administrator to set a nonempty password. No failed/unknown-name fallback to Guest. |
| CiukiUser, uid 1000 | Only ready-made images that bypass Setup; groups 544+545. It is an ordinary account despite the distribution default name. | Enabled, passwordless, boot auto-logon as expressly required by the owner. |
| Setup custom user, uid 1000 | Mandatory on Setup installations, groups 544+545. CiukiUser is not also created. | Enabled; proposed nonempty password required. First ordinary session requires logon. |
| Later users, uid >=1001 | Administrator creates as Standard (545) by default or Administrator (544+545) explicitly. | Nonempty password required before enabling. Disabled records cannot log on. |

Proposed limits retain capacity for all 16 G32 users plus the two built-ins:
18 account records, excluding SYSTEM. Creating beyond the bound returns ENOSPC
without consuming an ID. Allocate monotonically only as part of a successful
creation transaction. Persist the allocation high-water mark across deletion;
deleted IDs/profile roots are never assigned to another account. ID exhaustion
refuses creation. The kernel half must reconcile the invalid-UID sentinel with
libc before the last possible 32-bit ID can be allocated.

Names are 1-31 ASCII bytes from `[A-Za-z0-9_-]`, compared case-insensitively,
preserving spelling. Reject duplicates and reserved names SYSTEM,
CiukiAdministrator, CiukiUser, Guest, Administrators, Users and Guests for custom
creation/rename; existing distribution/built-in names are assigned internally.
Setup's older 32-character label rule is superseded for account names. Computer
names retain the LAN rules. A user rename changes a label, never its uid, SID or
profile root. Pictures are bounded validated image references, never executable
content; the default uses existing approved assets without regenerating Ciuki.

Reject deletion, disablement and type changes for the active account (EBUSY).
Reject any operation leaving no enabled, password-configured normal-boot
administrator; the directed passwordless ready-image administrator counts until
it is deliberately replaced. Disabled recovery uid 500 does not count as a
normal-boot administrator. Other administrators can make changes after the user
logs off. Password/picture changes do not replace an immutable process credential.
Deletion retains files and protected owner metadata; the UI explains this and
offers administrator-managed backup separately. Guest data is persistent but
shared between everyone using that one account; do not imply individual privacy.

### 3.2 Password and retry policy

Proposed policy: 8-63 printable ASCII bytes, case-sensitive, including spaces;
no trimming, silent truncation, forced composition, automatic expiry or password
history. Old/new/confirmation fields are masked and cleared after completion,
cancellation and error. Self-service password change requires the old password;
an administrator may reset another inactive user's password through the service,
with confirmation. The account service never returns the stored verifier.

The only enabled empty-password exception is the owner's ready-image CiukiUser.
Changing its password disables auto-logon durably; removing that password is not
offered as a general account-management feature. An unconfigured, disabled
CiukiAdministrator or Guest has no verifier and cannot authenticate: unset is not
an empty password. Guest enablement and all custom accounts require a password.
Imported blank G32 passwords require credential enrollment (section 6), not an
enabled passwordless account. These are stricter than XP's zero minimum (R8).

Keep PBKDF2-HMAC-SHA-256, 16 fresh entropy bytes for each new/reset password,
32-byte derived verifier and constant-time comparison. Initial new-record
iteration count is 10,000, preserving the current unbenchmarked G32 setting;
no reduction or strength/performance claim is made. Store the per-record count.
Before release, the lead measures derivation latency on the Pentium III profile
and real T23, with at most one worker derivation at a time; target <=1 second per
interactive verification is proposed. A changed count needs recorded evidence
and a reviewed format bound. Migrate old verifiers without needing plaintext;
rehash after a successful login when parameters or password length need updating.
Entropy failure aborts creation/reset with an error; it never substitutes a
timestamp or pseudo-random fallback. Wipe intermediate/password buffers.

Match the XP local default: persistent account-lockout threshold 0, no permanent
lockout, duration/reset-window not applicable (R8). This avoids letting a local
program disable every administrator. Rate limiting is separate: the service
serializes verification and enforces a global minimum one-second retry interval
after a failed attempt, measured by monotonic time; it queues no unbounded work.
Unknown/disabled users do dummy bounded verification and receive the same public
error as a bad password. Apply this to login, unlock and password-change checks;
do not call it XP's lockout counter behavior. Service restart can reset the delay;
it does not enable accounts or bypass verification. No network authentication is
exposed by this service.

### 3.3 Proposed v2 format and storage

The account service exclusively owns `/System/Security/accounts.db` on the system
volume, with `.new`, `.txn`, `.bak` siblings during replacement. All names,
backups, migration maps and journals are SYSTEM-only under the kernel policy;
FAT hidden/read-only bits and POSIX mode 0600 alone are insufficient. Public
name/picture/uid/group snapshots are a filtered service interface, not a
world-readable copy of this file. Profile ownership metadata is separately owned
by the kernel contract; this document does not define its encoding.

Proposed serialized layout is little-endian, explicitly encoded, not a dump of C
struct padding. A 96-byte header precedes `record_count` 256-byte records, exactly,
with no trailing bytes. This is a disk schema, not a public SDK/kernel ABI.

| Header offset | Field |
| --- | --- |
| 0 | 8-byte magic `CIUKISEC` |
| 8 | u32 version = 2 |
| 12 | u32 header_bytes = 96 |
| 16 | u64 generation, positive and increasing; no wrap |
| 24 | u32 next_uid; header flags carry exhaustion when no next id exists |
| 28 | u32 record_count, maximum 18 |
| 32 | u32 flags: migration_complete bit 0, uid_exhausted bit 1; other bits zero |
| 36 | u32 record_bytes = 256 |
| 40 | three u32 machine-SID components, fixed for the installation |
| 52 | u32 origin: 1 ready image, 2 Setup, 3 migrated install |
| 56 | u32 auto_logon_uid: 0 off, 1000 only for the directed ready-image exception |
| 60 | u32 reserved = 0 |
| 64 | SHA-256 of the entire encoded file with these 32 digest bytes zeroed |

| Record offset | Field |
| --- | --- |
| 0 | u32 uid |
| 4 | u32 flags: enabled bit 0, built_in bit 1, password_set bit 2, ready_blank_exception bit 3, change_password_required bit 4; other bits zero |
| 8 | u32 group bitmap: Administrators bit 0, Users bit 1, Guests bit 2; only combinations 3, 2, 4 are valid |
| 12 | u32 KDF id: 0 unset, 1 PBKDF2-HMAC-SHA-256 |
| 16 | u32 iterations: 10,000 initially; zero only when KDF unset |
| 20 | u32 original G32 profile id, zero if not imported |
| 24 | name[32], NUL-terminated and zero-padded |
| 56 | salt[16] |
| 72 | verifier[32] |
| 104 | picture[96], NUL-terminated relative reference validated against the picture catalogue/profile picture directory; empty means default |
| 200 | reserved[56], zero |

Records are sorted by uid. Validate uniqueness, known groups/flags, mandatory
built-ins, name rules, allocation high-water, origin/auto-logon consistency and
all length/arithmetic bounds before any allocation or KDF work. `password_set`
distinguishes a valid empty verifier on the directed exception from unset. Unset
salt/verifier fields are zero; new empty-exception provisioning still uses a
fresh salt. Imported records use migration validation rules for short old
passwords. A checksum detects corruption, not malicious offline tampering.
Unknown versions and KDF parameters fail closed. The initial accepted KDF count
is exactly 10,000; increasing it requires updating the accepted bounds first.

### 3.4 Serialization, replacement and recovery

Use one live SYSTEM service and an in-memory serialized mutation queue. Clients
cannot write the store or acquire a persistent management lock. Associate pending
requests with authenticated channel identity and request generation; client death
cancels uncommitted work. The kernel must prevent a second live account-service
instance. Service death releases its live lease; restart recovers storage before
accepting any logon or mutation. A leftover G32 `users.lock` never blocks v2.
Do not remove a purportedly live lock merely because a PID/time looks old.

Under that single writer, validate a complete new generation, write `.new`,
flush/fsync, write/fsync an intent naming old/new generations, size and checksum,
then retain the old file as `.bak` while installing the new canonical file.
Use the registry contract's native EEXIST-aware replacement and volume metadata
barriers; never unlink the sole valid database to make room. Complete durability
and cleanup before acknowledging success. Restart validates candidates against
the intent, completes a valid intended installation or restores the previously
valid generation for an unacknowledged operation. An acknowledged password reset
must not later silently revive an older password through a leftover backup.
Contradictory/corrupt recovery state stops authentication and management with a
diagnostic; missing files on a previously provisioned system never create a new
passwordless default account.

Account creation/deletion also needs profile ownership publication. The service
must stage inaccessible profile data, durably reserve the id/owner, and publish
the enabled record only when kernel owner installation succeeds. Interrupted
creation can leave a disabled reservation or orphaned directory, never an
accessible ownerless tree. Kernel half owns the exact coordinated publication
and recovery interface (Q3); the v2 transaction cannot pretend to provide
cross-file atomicity on its own. All I/O and KDF work runs off UI callbacks.

## 4. Logon, sessions, lock and power

### 4.1 Service interfaces and invariants

PID 1 starts a pinned, protected SYSTEM account/logon service before any ordinary
desktop. Its endpoint is not a generally callable spawn-as-user primitive.
Only the trusted logon UI may submit authentication/enrollment/unlock requests;
ordinary account-management clients get authorized operations, filtered reads and
self-password change. Kernel-provided sender identity, never a request's uid,
environment or window title, authorizes each call. Every mutating request has a
bounded copied payload and expected database generation; stale views return
EAGAIN for refresh. Return EACCES/EPERM for denied operations, EBUSY for active
accounts/transactions, EINVAL for malformed fields, ENOSPC for bounds, EIO for
storage corruption, and ENOMEM for allocation failure. No new panic path.

On successful authentication the service obtains a session through the kernel
half, loads the profile and starts a fresh desktop as that uid. SYSTEM fds,
secrets and credential-creation endpoints are never inherited. All ordinary
children, including workers, native programs, Win32 clients and DOS VM owners,
inherit the user's credential. A SYSTEM broker must carry authenticated caller
context to perform permitted work, not execute client-supplied paths under its
own unrestricted authority.

Proposed observable states are UNPROVISIONED, LOGON, AUTHENTICATING, ACTIVE,
LOCKED, ENDING and STOPPING. Failed authentication stays at LOGON/LOCKED; failed
profile/hive load does not enter ACTIVE. Fresh desktop publication is all-or-none.
Service/UI crash, resume failure or database error never selects a default
profile. Keep a protected error/retry/shutdown screen when login is unavailable.

### 4.2 Welcome and auto-logon

The English Welcome-style list contains enabled ordinary accounts and Guest only
when explicitly enabled. Hide uid 500 except in Safe Mode or after explicit
administrator enablement. Provide a typed-name alternative; it applies exactly
the same enablement/password checks. Unknown names cannot create accounts.
Escape/Cancel clears a credential attempt or returns to the list; it never starts
a session. The Close button cannot dismiss the logon gate into a desktop.

Only a ready image with origin=ready, enabled uid 1000 named CiukiUser, the
ready-blank exception and auto-logon=1000 can boot directly into that account.
Authenticate that explicit account through the normal session path; do not treat
an absent store as permission. Password/type/enablement/name changes that break
this tuple disable auto-logon in the same transaction. Safe Mode, explicit Log
Off and Lock MUST NOT auto-logon. Other accounts and Setup installs have no
auto-logon setting in this step. This owner-mandated blank account is the stated
exception to the general valid-name/nonempty-password rule when Guest is disabled.

### 4.3 Lock, saver and resume

Implement one trusted lock operation for Start > Lock, the trusted keyboard lock
entry point, password-protected saver dismissal and password-on-resume. The
kernel half must revoke ordinary display/input/full-screen access before the
trusted UI becomes visible, reject synthetic unlock input and prevent mapped
framebuffer access from exposing or covering credential entry. Pause/revoke
presentation leases; restore them only after authenticated unlock. The protected
UI never sends password text through normal desktop messages, clipboard,
screenshots or logs. A kernel-owned secure-attention entry point must be available
to reach the trusted UI; its exact binding is reconciled with the kernel half.

Unlock requires the current session owner's password. No arbitrary administrator
can unlock into another user's desktop in this step; the owner can end that
session through the orderly/confirmed termination route and then log on afresh.
Cancel or failure remains locked. Application state stays alive on a lock;
notifications/painting/input remain concealed. A crashed ordinary desktop does
not make the trusted lock disappear. Account-service restart invalidates stale
authentication replies and preserves the locked state.

Passwordless CiukiUser cannot provide a password-protected lock. Show "Set a
password to lock this account" and open its password-change path; do not present
an empty-password gate as security. Savers can still conceal the screen. Once a
password exists, propose saver password and resume password enabled by default;
the user may change their own preferences. Preview never changes lock policy.
Resume protection is armed before suspend, before any application input/display
is restored. Failed suspend unwinds normally; a completed resume remains locked
until verification. Unqualified standby remains unavailable for the existing
hardware reasons; these requirements do not claim a new ACPI/APM capability.

### 4.4 Log Off, Switch User and shutdown

Keep one interactive session. Switch User/Fast User Switching is out of scope:
the compositor, full-screen leases and memory budget have no qualified concurrent
session model. Do not show an enabled Switch User action. To change users, Log
Off then authenticate the next user.

Log Off uses G3 native query/veto and Win32 WM_QUERYENDSESSION with
ENDSESSION_LOGOFF (R10). A save prompt/veto leaves the same ACTIVE session and
identity. After consent, stop session processes/VMs, flush personal settings and
hives, revoke all session handles/grants and destroy the desktop. Start LOGON
only after that cleanup; no background process of the previous user survives as
the next user. A confirmed force-logoff warns about unsaved data. Flush failure
is visible and offers retry or explicit force; it does not report saved data.

The trusted logon screen offers Shut Down/Restart, even without authentication,
as local machine policy. It submits only those fixed requests through the SYSTEM
service and the existing orderly power path, never a shell. It cannot enter an
ordinary session. Logged-in Users/Guests may request local shutdown/standby through
the session UI and veto-aware broker; changing machine power policy is separate.
Remote shutdown is absent. Logon-screen shutdown does not imply permission to
format disks or change accounts.

## 5. Setup and ready-made image provisioning

### 5.1 Interactive installation

Setup must collect a custom username, its password/confirmation, and
CiukiAdministrator password/confirmation. Both proposed minima are eight
characters. The owner mandated creation of a custom administrator and choosing
the recovery password; requiring nonempty passwords makes the new authentication
boundary meaningful. This is a proposed policy choice, not a claim that the
owner specified a particular minimum. Reject reserved names, including CiukiUser;
Setup installs never create that distribution-default account.

Extend screen 7 or add a keyboard-accessible account page with the same validated
answers model. Review pages show only names and "Password set", never passwords,
verifiers or salts. Do not save secrets into System\\Identity, SETUP.STATE,
SETUP.LOG, manifest summaries, serial output or BOOT.CFG. Keep proposed account
data in bounded worker-owned buffers and erase it on Back/Cancel/finish/error.
Collecting or cancelling still performs no ATA writes before the existing
Install confirmation.

During libsetup's configuration phase, trusted boot-only Setup provisioning
creates uid 500 (disabled, password configured), uid 501 (disabled, unset), uid
1000 (enabled custom administrator), owner metadata, profile directories and
registry defaults on the destination prefix. It must not modify source-media or
host-session accounts. Destination credentials/owners are verified before the
installation is sealed or firstboot can complete. Retrying configuration resumes
the same reserved uid, never creates duplicate accounts. A failed credential
commit leaves installation incomplete and unbootable into an ordinary desktop.

After installed-disk boot, SYSTEM logon runs before the existing firstboot
wizard. The authenticated administrator's desktop runs the wizard without SYSTEM
credentials; privileged machine actions go through authorized services. The CD
remains Setup-only and its rescue tools remain read-only for ATA (no hidden
password-reset write path).

### 5.2 Unattended installation and images

Propose SETUP.INF schema version 2. Version 1 files with only UserName are no
longer complete account answers. Retain disk/component/network rules, require
UserName and both passwords. The initial safe implementation can prompt for the
two passwords when no secure answer transport exists; explicitly unattended mode
then stops with `status=missing_answer` before any disk write. Do not silently
use blank defaults or place plaintext secrets in a distributed sample answer
file. A fully unattended secret transport/verifier import requires the lead's
decision Q5 and its own strict parser/secret-redaction tests; no unreviewed INF
key is declared implemented by this document.

`scripts/build_image.py` must distinguish ready images from Setup provisioning
templates using explicit build metadata, not absence of users.dat. Ready images
carry enabled passwordless CiukiUser uid 1000 plus disabled reserved accounts.
CiukiAdministrator initially has an unset verifier on such images: its password
was never chosen in Setup. It cannot authenticate even in Safe Mode until an
authenticated administrator sets it. The account applet offers that configuration
after setting CiukiUser's password. This recommendation needs Q2; no distributed
known password or generated secret hidden in a public image is acceptable.

A Setup payload must not accidentally copy the ready-image auto-logon/account
database onto the destination. Shared payload generation, Setup catalogues and
static image checks must recognize the provisioning distinction while retaining
one canonical runtime build and the approved Ciuki identity/assets.

### 5.3 Lost passwords

If a normal administrator password is lost, Safe Mode shows CiukiAdministrator;
after verification with its configured password, it may reset an inactive normal
account. Ordinary Safe Mode does not enable Guest or skip authentication. Q1
must decide whether disabled uid 500 receives this narrow recovery logon exception.
If its password and every other administrator password are lost, this step
provides no passwordless bypass: read-only rescue can save logs; external backup
and reinstall are the supported recovery outcome. Password reset disks, offline
database reset tools and an authenticated write-capable CD recovery mode require
a separate directive. This preserves the existing read-only rescue contract.

## 6. Profiles, migration and protected storage policy

### 6.1 Paths and permissions consumed from the kernel half

Keep numeric roots `/Windows/Profiles/u%08x` to avoid a bulk data move and
username/path ambiguity. New roots use the new uid; imported roots can retain
their old numeric suffix through protected owner mapping. There is exactly one
canonical root per uid, looked up through the service/kernel metadata. The
suffix or a client-provided HOME is never proof of ownership.

| Managed system-volume tree | SYSTEM / administrator | Standard / Guest |
| --- | --- | --- |
| Volume root | SYSTEM and administrator may manage ordinary entries. | Read/traverse public entries; no arbitrary create/write/delete. Deliberately stricter than XP root folder-creation exceptions (R5). |
| `/System/Security` and every account/owner journal | SYSTEM only. Administrator uses validated service requests. | No read, enumerate or write. |
| `/System`, `/Programs`, `/Windows` including SYSTEM/SYSTEM32, fonts and machine settings | SYSTEM services and authorized administrator maintenance; existing protected builtin-store restrictions on ordinary Win32 writes remain. | Read/execute public payload; no machine mutation. More-specific private profile rules override public read. |
| `/Program Files` including Common Files | Administrator installs/updates; system DLL replacement still obeys F5 builtin rules. | Read/execute, no write/create/delete. |
| `/Windows/Profiles/<mapped root>` | SYSTEM; administrators may manage user files through policy-authorized operations. | Owner has read/write/create/delete in ordinary profile data; other Users/Guest denied; Guests only their own Guest root. Protect the profile root/owner binding against reassignment. |
| `/Windows/Profiles/Default` | SYSTEM/administrator manage template. | Read-only template, no active identity. |
| `/Documents` and `/Documents/Desktop` after migration | Becomes an explicitly shared legacy data tree; administrator manages its publication. | Proposed Users read/write shared documents; Guest read-only. It is not everybody's private home. |
| Global Start Menu/Desktop defaults and `/System/Registry` | SYSTEM/administrator updates machine configuration. | Read; personal overrides go under the profile. Never let a shared shortcut/data directory inject SYSTEM startup code. |
| Per-profile Documents/Desktop, Favorites, Start Menu/Programs, Application Data, Local Settings/Temp, settings | SYSTEM/service and owning user. | Owner can edit; other accounts denied. Credential/owner records never reside here. |

This is the required user-visible matrix, not a redefinition of kernel resolver
algorithms. All native/Win32/DOS routes, 8.3 aliases, case variants, volume aliases,
relative paths, `..`, moves and inherited handles must obey the kernel half.
Source and destination authorization both matter on rename. Raw volume/device
access must not let a user edit protected files underneath the policy. Unmapped
private roots or unavailable owner metadata are denied, not treated as shared.

Removable FAT media are explicitly shared transport, not per-user private
storage: Users and administrators may read/write files; Guest is proposed
read-only. Ordinary eject/safe-remove requests use the existing busy/flush checks;
format/partition/raw writes require administrator and existing device/Setup
constraints. NTFS remains read-only, CDs read-only. A foreign volume cannot
replace trusted programs or confer SYSTEM identity by copying a pathname.

### 6.2 Registry and per-profile state

The native text registry and Win32 CRG stores remain distinct formats with their
existing durability guarantees. Rebind native personal Desktop/settings roots
and Win32 USER.CRG slots to the selected profile. Keep native System/Programs
machine roots protected; Users security data is service-managed, never a writable
role-selection file. Machine Hardware inventory stays read-only for everyone.

Win32 HKLM is public-readable except security secrets and administrator-writable
except SYSTEM-only/internal state. Do not expose account verifiers through SAM,
SECURITY or a CiukiOS equivalent. HKU enumerates only authorized public account
information and loaded hives; a Standard/Guest client can open its own SID branch,
not another user's data. HKU\\.DEFAULT is a SYSTEM-owned template/logon default,
never a fallback personal hive after authentication failure. HKCU is bound to
the authenticated uid for the lifetime of that process. HKCR retains the F5
machine-Classes view in this step; writes need administrator. HKCC public queries
remain read-only, with machine display changes routed through policy. No arbitrary
Win32 ACL-setting operation may widen these fixed policies.

`w32srv` must enforce key access on the authenticated caller, regardless of its
own service identity. Registry handles retain uid/session/access scope and are
invalid after teardown; another process cannot adopt a raw numeric handle.
The storage worker serializes writes per hive/root and releases dead-client
leases, addressing the native registry's current single-writer requirement.
Direct backing-file access must not bypass it: authoritative hives and their
intent/backup files need service-only subdirectories, even within an otherwise
user-writable profile. Applications receive HKCU/libreg operations through the
service, not writable raw hive files. A writable parent must not let the owner
rename, remove or relocate a service-only hive directory to bypass its policy;
that is a required kernel-half subtree-protection case. Existing personal non-registry settings
can remain owner-writable where generation-safe concurrent access is supported.

### 6.3 G32 conversion

Migration is one SYSTEM-controlled, restartable transaction before an ordinary
session starts. Version 1 G32 records carry no security roles and were stored on
a shared filesystem; possession of one old password is not authority to promote
that user or to approve machine migration.

Propose an explicitly initiated upgrade/enrollment workflow, pending Q4. Require
the operator to choose the first custom administrator and recovery password;
assign the selected administrator uid 1000. Import every other G32 named profile
as Standard, assigning fresh uids in old-id order. If an old name collides with a
reserved name, require a replacement label while retaining its files; never
reinterpret an old SYSTEM/Guest/CiukiUser label as a built-in account. Persist
old-id -> new-uid -> existing-root mapping, protect owners before enabling, and
do not recycle the old profile directory. Import at most the existing 16 named
users without truncation. If there are none, require creation of a new custom
administrator; absence of G32 data is not a ready-image signal.

Valid v1 salts/verifiers remain PBKDF2-SHA-256/10,000. New role metadata is not
inferred from them. Nonempty imported passwords shorter than eight are accepted
only by the trusted enrollment gate, which requires a conforming replacement
before any desktop starts. A verifier of the empty string is detected during
conversion; disable that imported account until an administrator enrolls it with
a nonempty password. Corrupt records/checksums stop conversion, preserve original
files for repair and never reset passwords silently. All enabled imported
accounts use credentials only after the migration commit.

Preserve old profile data/settings. Move the original users.dat, users.new and
users.lock into SYSTEM-only migration evidence; no verifier copy remains in
shared settings. The service ignores stale v1 locks once it owns v2. Record a
protected migration marker/high-water map so retries cannot import twice or make
fresh administrators. Missing v2 state after migration is an error, not a request
to reimport publicly editable v1 data. Rollback before publication restores old
data under a noninteractive recovery state, never the old Cancel-to-default gate.

Keep `/Documents` shared until an administrator explicitly chooses what belongs
to whom; do not guess the owner of previously shared files or delete them. Shared
Win32 USER.CRG becomes a read-only migration input used to seed personal hives;
warn that formerly shared settings were not private. Copy per-user roots without
carrying machine-authority fields. An upgrade cannot claim new isolation until
old public secrets, writable machine roots and service startup paths are closed.

## 7. Control Panel and old authority mechanisms

### 7.1 User Accounts and machine settings

Merge Users and Passwords into **User Accounts**. Preserve the old Passwords
launch identifier as an alias to the current user's password page; remove G32's
"disable logon/use default profile" options. Administrators can create/delete
inactive users, rename, change type, reset passwords, select pictures and enable
Guest/CiukiAdministrator through the service. Standard users can change their
own password/picture and see their own type; administrator controls are disabled
with "You must be an administrator to change this setting." Guest can view its
account and change its password if policy permits, but cannot alter type,
enablement or other accounts. Re-check authorization when applying, not only
when opening a page. All unavailable actions have visible explanations.

| Applet/page/action | Standard/Guest view | Administrator action |
| --- | --- | --- |
| Device Manager / drivers / hardware enable-disable | Public inventory, read-only; modification disabled. | Authorized machine changes; trusted driver-install/update path only. |
| Network adapter/IP/DNS/DHCP configuration, global proxy/shares/lobby defaults | Status readable; machine edits and DHCP release/renew disabled. Per-user application networking continues. | Machine configuration and DHCP control; no new network-authentication provider. |
| Date/time and system time zone/RTC | Read clock, personal formatting/locale editable; clock/time-zone writes disabled. | Change system time/time zone through checked service. No Power Users tier. |
| Power | Battery/status and personal monitor/saver/resume preference controls usable; machine standby/disk timers, button/lid defaults disabled. | Change machine policy. Users/Guest may request local power actions via the session UI. |
| Display | Personal wallpaper/colors/theme/saver editable; read current mode. Machine mode/driver/acceleration/default-template changes disabled. | Change machine defaults/mode through the trusted display path. Per-session game display leases remain available to all account types. |
| Add/Remove Programs | Own-profile install/uninstall and published program list; all-users actions disabled with reason. | All-users install/uninstall and bundled program management. |
| Sounds, keyboard/mouse, regional, accessibility | Personal preferences editable; public device capability view. Machine driver/default-page changes disabled. | Machine defaults/device settings as applicable. |
| Registry Editor, startup recovery, ScanDisk, disk tools | Personal keys/files editable through scoped services; machine edits/repair/raw writes disabled. Public reports/read-only checks allowed. | Checked machine edits and repair; SYSTEM security records still service-only. |

These are proposed CiukiOS rights informed by XP, not a byte-for-byte clone of
every XP applet default. When a control combines personal and machine fields,
split its save request and storage scope; opening Control Panel never elevates
the whole process. Authentication/policy failures must be observable in direct
service and syscall tests even when the UI is bypassed.

### 7.2 Authority conversion inventory

| Existing authority/input | Mapping to the new model |
| --- | --- |
| PID 1/bootstrap, protected logon/account service | SYSTEM-only. Arbitrary credential creation, account-store I/O, trusted gate and account/owner publication stay here. Administrator membership does not grant uid 0. |
| `dosvm_capability`, `/Programs/dosbox` exact-path launch | Per-user VM creation; uid/session inherited, all guest I/O checked for that user. Existing binary/grant restrictions can remain as implementation-resource checks, never role grants. |
| `programs_capability`, bundled-program GAME_REMOVE/RESET/EXPORT | All-users removal/reset admin-only; per-user export/own data per-user. Audit each operation instead of elevating every DOS action. |
| `network_capability`, `dhcp_capability`, corresponding AUTHORITY/status bits | Machine network and DHCP mutation admin-only; status and ordinary sockets per-user. Existing trusted DHCP boot service remains SYSTEM with narrow internal work. |
| `time_capability`, `CIUKI_TIME_AUTHORITY` | System time/time zone admin-only; public time and personal formatting per-user. Status bit reflects actual rights, not binary name. |
| `recovery_capability`, `CIUKI_RECOVERY_AUTHORITY` | Boot/startup mutation and repair admin-only; reports/read-only checks per-user; protected boot-service work SYSTEM-only. |
| `setup_capability`, Setup mode and destination-disk lease | SYSTEM-only boot Setup, fixed destination and existing no-write-before-Install rule. A normal administrator launching setup cannot acquire this privilege by pathname. |
| POWER authorization from display/input grants | Per-user request of local power actions through session broker; machine policy admin-only; logon-screen fixed shutdown/restart SYSTEM-only UI. Possession of grants does not confer all power-policy rights. |
| `CIUKI_TASKS_FD`, `--authority-fd`, call 71 and signal/termination routes | Per-user termination within that uid/session with existing self/desktop/controller restrictions; admin may request ending another ordinary session/process under kernel policy. SYSTEM/PID 1/protected services remain protected. An inherited fd or forged argument cannot cross the role boundary. |
| Display/input grants, delegated Win32 presentation and master-mixer fds | Per-user/session grants; revoked/scoped by trusted lock/session transitions. Mixer access for the active session does not confer account, driver or machine-policy rights. |
| Win32 service/loader/compatibility-writer registration and trusted paths | SYSTEM registers trusted services; each request uses the caller's identity. App loading/extraction is per-user; machine shim database/admin configuration admin-only. Existing service endpoints must not turn a user-writable binary/path into authority. |
| Environment (`CIUKI_PROFILE`, HOME, USER, TMPDIR, CIUKI_* fd strings) | Per-user hints only; consult kernel credentials for authority and protected metadata for canonical profile binding. |

Keep legacy constants/SDK signatures where compatibility requires them; their
reported truth and effective scope must follow the new policy. Do not maintain an
independent path-derived administrator mechanism alongside credentials. The
kernel half owns the exact enforcement migration and its remaining authority
audit; no public numeric ABI is allocated in this document.

## 8. Win32, native and DOS interfaces

### 8.1 Win32 identity and token subset

Translate local uid u to `S-1-5-21-A-B-C-u`, where A/B/C are installation-stable
machine components, except SYSTEM -> `S-1-5-18`. Groups map to
`S-1-5-32-544/545/546`. Local lookup returns the configured computer name as
account domain, BUILTIN for built-in groups and NT AUTHORITY for SYSTEM.
Computer rename changes the display domain, not persisted SIDs. Component
provisioning/cloned ready images need Q6 before network identity is ever used;
there is no network SID trust in this step.

| API subset | Required behavior and limits |
| --- | --- |
| GetUserNameA/W | Authenticated process username, not `Ciuki`, USER/HOME or a desktop-selected string. Buffer size includes terminator; insufficient buffer reports required size and ERROR_INSUFFICIENT_BUFFER. No impersonation in this step. |
| OpenProcessToken | Obtain query-only handle for own process; other-process access requires the kernel's process-query authorization. Store uid/session scope in the real handle table. TOKEN_QUERY supported; adjustment/assignment/elevation rights refused. CloseHandle releases it. |
| GetTokenInformation | TokenUser, TokenGroups, TokenPrimaryGroup, TokenOwner, TokenType, TokenSessionId and TokenPrivileges from the kernel credential/operation-right view. Sized buffer queries, self-contained SID/pointer layouts and ERROR_INSUFFICIENT_BUFFER; unknown classes ERROR_INVALID_PARAMETER. No UAC TokenElevation story is fabricated. |
| CheckTokenMembership / IsUserAnAdmin | NULL token checks current effective identity. Administrators group is true only for 544, Users for 545, Guests for 546; no automatic admin true for every program. IsUserAnAdmin is a membership wrapper. R11 requires an impersonation token for a non-NULL CheckTokenMembership handle: return ERROR_BAD_TOKEN_TYPE for an explicit primary-token handle. A later directive may add query-only DuplicateToken(SecurityIdentification) if an installer fixture requires that form; it cannot confer impersonation/credential-assignment rights. |
| AllocateAndInitializeSid / FreeSid / IsValidSid / EqualSid / GetLengthSid and accessors | Retain/complete validated NT-layout data helpers. AllocateAndInitializeSid accepts at most eight subauthorities and never creates an account or privilege. Fail invalid SID/length/arithmetic without overwriting buffers. |
| LookupAccountSidA/W | Local SYSTEM, built-ins and current stored accounts only; correct name/domain sizing and SID_NAME_USE. Unknown SID ERROR_NONE_MAPPED; no network lookup. |
| LookupPrivilegeValue and TokenPrivileges | Expose only reviewed operation-right mappings used by the implementation; minimum system-time, local-shutdown and driver-management distinctions must agree with section 7. Do not claim SeDebug/SeTcb/SeAssignPrimaryToken to ordinary administrators. AdjustTokenPrivileges cannot grant missing rights; unimplemented adjustment fails explicitly. |

An implementation directive must freeze exact supported TokenPrivileges entries
against the kernel half (Q3); it must not publish a made-up Windows privilege list.
SCM, network logon, arbitrary token duplication/assignment, LogonUser,
CreateProcessAsUser, SetThreadToken and NT ACL mutation remain explicitly outside
this step. Existing refusal conventions must remain typed errors rather than
new blanket-success stubs. Token/registry requests use authenticated transport;
client-side membership tests are convenience checks, never enforcement.

### 8.2 Shell folders and registry binding

Load profile and hive before starting Win32/native desktop clients. Populate
HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders
and Shell Folders consistently with SHGetFolderPath/SHGetSpecialFolderPath and
native common-dialog places. User-overridden folder locations are validated under
that user's rights; editing a registry string cannot grant access elsewhere.

| Folder/API/environment | Per-user mapping |
| --- | --- |
| CSIDL_PROFILE / USERPROFILE | Canonical protected profile root |
| CSIDL_PERSONAL / native HOME | `<profile>/Documents` (English product label remains Documents) |
| CSIDL_DESKTOPDIRECTORY | `<profile>/Documents/Desktop` |
| CSIDL_STARTMENU / CSIDL_PROGRAMS / CSIDL_STARTUP | `<profile>/Start Menu`, its Programs and Programs/StartUp |
| CSIDL_FAVORITES | `<profile>/Favorites` |
| CSIDL_APPDATA / CSIDL_LOCAL_APPDATA | `<profile>/Application Data`, `<profile>/Local Settings/Application Data` |
| GetTempPath / TEMP / TMP / TMPDIR | `<profile>/Local Settings/Temp`; no writable shared Windows/TEMP fallback |
| Common Programs/Desktop/Application Data | Protected machine menu/default locations and explicit shared data under `/Documents/Shared`; SYSTEM/administrator publish code/menu entries. Shared application data is not a trusted executable search path. |

HKLM writes and all-users shell registration need administrator. HKCU writes
belong to the process user. An installer child uses the same credential as its
parent, including when launched from an extracted temporary executable. A service
must not bind every request to its own SYSTEM HKCU or the last interactive user.
Flush/unload hives at logoff; a next user gets fresh handles, temporaries and
clipboard/session state. Preserve existing file-system and registry durability
limits; authorization does not make FAT replacement atomic.

### 8.3 Installers and old games

VISE, InstallShield and NSIS can install for all users when already run by an
administrator, including CiukiUser on ready images. Standard users can extract
and install entirely within their profile if the installer supports it. An
all-users request fails with ERROR_ACCESS_DENIED and the shell explains that the
user must log off and log on as an administrator. This is usable without adding
a Run as credential broker to this step. Installer names/manifests and Windows
98 version probes never cause elevation. Test the original engines on the
lead's private fixtures without publishing payloads.

For games writing beside their executable, the default rule is XP-style failure
under protected Program Files for Standard/Guest. The supported compatibility
route is a **per-user installation/copy into that user's profile**, where its
program-local config/save writes are authorized. Administrators retain their
normal write rights for non-system game directories. Do not globally make
Program Files writable or silently introduce Vista VirtualStore/HKLM
virtualization. A title needing shared mutable data must use an explicitly
reviewed shared-data directory; no privileged process loads code from it. More
ambitious per-title data redirection requires separate review and evidence.

GetVersion/GetVersionEx continue reporting the existing Windows 98 SE identity
unless an existing named version shim applies. New security APIs are an additive
compatibility subset; Windows 98-era software sees the actual username, personal
folders and access-denied errors for unauthorized machine writes. A version shim
does not alter credentials. The older F5 blanket `C:\\System` denial for ordinary
Win32 file writes remains; machine updates use the trusted maintenance path.

### 8.4 Native libc and DOS

Provide getuid/geteuid with equal 32-bit values from immutable credentials;
getgid/getegid give Users (545) for ordinary administrators/Standard, Guests (546)
for Guest, and a documented internal SYSTEM primary group resolved with the
kernel half. getgroups returns actual supplementary membership including 544 for
administrators. uid 0 means SYSTEM to ported programs; uid 500/1000 administrator
is not POSIX root. Programs needing an administrator check must use the group/
native rights query, not assume every administrative account has uid 0.

Provide getpwnam/getpwuid and `_r` variants through filtered service lookup:
name, uid, primary gid, canonical profile/home and the supported shell; no password
verifier. If newlib has a pw_passwd extension it is `*`, not a hash. Unknown user
returns the standard not-found result; `_r` uses caller storage and ERANGE for
short buffers. Credential-changing setuid/setgid/setgroups/setresuid-style APIs
are unsupported or fail EPERM when asked to change identity; no executable mode
bit grants privilege. There is no public switch-to-SYSTEM libc operation.
The native API remains a POSIX subset, not full POSIX credential conformance.

Every DOS VM captures its creator's uid/groups/session. DOS EXEC children remain
in that VM identity; no DOS username environment variable or guest kernel field
can change it. The VMM's shared VFS enforces profile/machine rules on handle,
FCB, LFN, directory search, metadata and sector routes. Denial maps to DOS access
denied (5) and the existing extended-error conventions, without a local-FAT
fallback. Do not replace the unified FreeDOS/V86 design with NTVDM semantics.

Keep existing C:/D:/E: volume roles and drive-letter enumeration; no per-user
drive remapping/SUBST in this step. Start new VMs with a user-owned working
directory and per-user temporary/save paths where supported. C: still names the
managed system volume, with access filtered by credentials, not a private image.
Ordinary DOS game exports must not reveal other profiles or raw protected volume
contents. No DOS memory limit is changed by this security work.

## 9. Failure modes and integration invariants

1. Missing/corrupt accounts or owner metadata on a provisioned install prevents
   logon. There is no default-user, Guest, `.DEFAULT` hive or G32 fallback.
2. Bad password, disabled account, typed hidden name, Escape, Close, failed worker,
   forged UID and stale authentication reply never publish a session.
3. SYSTEM-only account/owner data stays inaccessible through native, Win32, DOS,
   alias, device and broker routes, including backup/intent files.
4. A failed account mutation leaves the previous durable state authoritative or
   an explicit noninteractive recovery state. Success is not returned before
   ownership and credential persistence have reached their shared commit point.
5. Ordinary desktop/app code never retains SYSTEM credentials, privileged fds or
   display/input access during logon/lock. Auto-logon is still an actual uid 1000
   session, never a continuation of a SYSTEM desktop.
6. Group changes take effect in new credentials after logoff. The service rejects
   active-account role/enablement deletion changes rather than mutating running
   immutable credentials or leaving stale administrator sessions alive.
7. Logout veto retains current identity/state; successful logout ends all that
   session's native/Win32/DOS processes before another desktop appears.
8. UI disabling is supported by service/kernel checks. Applet names, executable
   basenames, SIDs built in client memory and environment variables confer no right.
9. Entropy, storage, memory and service errors return bounded failures with useful
   English messages. Diagnostics contain operation/result/uid/session/generation,
   never passwords, hashes, salts or credential-bearing answer-file content.
10. Retain the approved Ciuki portrait, splash photograph, Tango icon family and
    `Classic computing, one woof at a time.` in Setup and desktop. No asset
    regeneration, new panic path, per-machine image or memory-policy change.

## 10. Implementation directives and acceptance

This is the proposed non-kernel sequence; the lead assigns final directive names
and reconciles it with the parallel kernel sequence before code starts. The
account/owner/trusted-gate kernel prerequisite must land before security is
advertised. Each implementation needs host evidence and lead review; this design
task runs no host suite.

| Proposed directive | Deliverables and dependencies | Host acceptance |
| --- | --- | --- |
| sec-user-01 — account service/store | Kernel credentials, authenticated IPC, service-only paths and live lease first; v2 codec, filtered reads, password operations, serialization, journal recovery, role/last-admin rules. | Known PBKDF2/HMAC vectors independently checked; malformed/duplicate/overflow records, every write/fsync/rename/death cut, concurrent/stale writers, entropy/OOM errors, no leaked buffers or stale lock; 16-user import capacity. Deny forged admin/SYSTEM and direct-store access. |
| sec-user-02 — Setup/image and migration | sec-user-01 plus coordinated kernel owner publication. Interactive credential pages, destination provisioning, ready-image distinction, protected migration maps and firstboot ordering. Q1/Q2/Q4/Q5 resolved first where applicable. | Zero ATA writes before Install, missing/invalid answers, redacted summary/logs, cancel/retry, destination-prefix isolation, every migration cut, reserved-name/blank-password handling, no accidental CiukiUser on Setup installs, no ready-image store overwrite. |
| sec-user-03 — trusted logon/session | Kernel exclusive gate/secure attention/session teardown prerequisite, then Welcome/list/auth/auto-logon, fresh user desktop, Lock and Log Off. | Gate state-machine production tests for Escape/Close/disabled users, stale replies and crashes; full-screen lease/input/framebuffer revocation; logout veto and cleanup; no SYSTEM fd inheritance. |
| sec-user-04 — applets/personal state | sec-user-03; User Accounts merge, authorization messages, machine/personal save split, G10/G11 password lock hooks. | Production UI/service tests for every operation-matrix row as Administrator/Standard/Guest; self-password old/new/confirm; disabled-control text; saver preview/dismissal/resume ordering and crash/error gates. |
| sec-user-05 — native/Win32/DOS consumers | Kernel query/operation rights plus sec-user-01/03; newlib account subset, token queries, w32srv hives/CSIDL, existing authority conversion, DOS context. | Actual service/facade tests under three identities; short buffers/bad SIDs/foreign handles, truthful membership, HKLM denial/HKCU separation, no broker confused deputy, inherited credentials, standard-user installer failure and own-profile install/game success, DOS I/O denial. |
| sec-user-06 — integration/evidence | Prior directives, complete authority census and reconciled docs. Add runner cases/fixtures without build variants. | Regression host suites, docs/ABI/reference checks as needed, then lead's canonical image and checklist below. Inventory remains partial until the relevant evidence exists. |

Existing runnable host entry points to extend in those directives are listed
below. Run sequentially, prefixed with nice; do not override sanitizer policy or
claim their current assertions already establish security. New service/kernel
cases must be wired into their owned host runners, not left as documentation.

```sh
nice -n 19 python3 -B -m unittest tests.host.desktop.test_users
nice -n 19 python3 -B -m unittest tests.host.desktop.test_window_model
nice -n 19 python3 -B -m unittest discover -s tests/host/win32 -p test_registry.py
nice -n 19 python3 -B tests/host/setup/run.py
nice -n 19 bash scripts/test/host_libsetup_tests.sh
nice -n 19 bash scripts/test/host_kernel_tests.sh
nice -n 19 bash scripts/test/host_fs_tests.sh
```

If a later directive changes abi.h or a public SDK header: extend
tests/host/abi_layout_test.c for every new record/constant, use one struct field
per line in abi.h, run the ABI section of host_kernel_tests.sh and regenerate
with `python3 scripts/gen_api_reference.py`. If vendored code includes a .c file,
check defined external symbols with nm. Those conditions do not occur in this
documentation change. Existing runner sanitizer choices are inherited; this
contract adds no leak-disable option.

### 10.1 Exact lead QEMU checklist (future implementation gate)

No QEMU evidence is produced by this contract. The lead, after implementation and
green sequential host suites, builds one canonical image for a source state and
records its SHA-256. Run one QEMU at a time under the project's capped runner;
use COW overlays, never copies/variants of the base image. Hardware/Pentium III
latency and physical suspend confirmation remain separate. The `sec-users` suite
and IDs below are **proposed runner work for sec-user-06**, not existing commands.

After wiring that suite, exact planned runner invocations, each sequential:

```sh
nice -n 19 python3 scripts/test/run.py f0-smoke --profile qemu-desktop-1998
nice -n 19 python3 scripts/test/run.py sec-users --profile qemu-desktop-1998
nice -n 19 python3 scripts/test/run.py sec-users --profile qemu-t23
```

The lead checks approval of this new security suite under the current QEMU policy
before running it; this text does not expand the existing allowed suites. Setup
cases use the canonical CD and a separately blank destination fixture through the
runner's installation path. Every case records base/ISO hash where used,
profile, uid/session, action/result and expected denial, with secrets redacted.

| Case | Exact action and pass criterion |
| --- | --- |
| SU-01 ready image | Normal boot: one ACTIVE uid 1000 CiukiUser desktop, groups 544+545; Guest/500 disabled, no SYSTEM credentials/fds in desktop. Explicit Log Off stays at Welcome. |
| SU-02 Setup | Cancel/missing/short/mismatched password before Install: zero ATA writes. Install custom Alice and recovery password: uid 1000 Alice, no CiukiUser, 500 disabled/password set; installed boot requires Alice authentication before firstboot. |
| SU-03 logon refusal | At Welcome try Escape, Close, blank name, unknown name, wrong password, disabled Guest and typed disabled 500 in normal boot. No desktop/session publication; generic refusal, bounded retry. |
| SU-04 roles | As administrator create Bob Standard and set Guest password/enable Guest. Log off/login each. Native/token queries show 545 versus 546; a copied/renamed control/setup binary or forged environment/authority fd cannot elevate. |
| SU-05 object policy | Bob/Guest attempt account-store/owner/hive backup reads; write Windows/System, Programs, Program Files, machine settings and Alice profile using native/Win32/DOS long/8.3/case/relative aliases. Denied consistently; own-profile writes succeed. Test both rename endpoints and raw-device bypass refusal. |
| SU-06 applets | As Bob view device/network/time/power/display/default-program pages, attempt disabled edits through UI and direct transport. Machine writes denied, personal edits persist only to Bob. Alice's settings stay unchanged. |
| SU-07 profile/Win32 | Alice then Bob: different Desktop/Documents/CSIDL, temporary directory, HKCU value and GetUserName; HKLM writes denied to Bob, accepted only as authorized administrator. Cross-user/old-session token and hive handles invalid/denied. |
| SU-08 installers/games | Administrator completes the selected VISE, InstallShield and NSIS fixtures with inherited credentials and correct menu/Uninstall scope. Bob's protected all-users install fails visibly; supported own-profile install and beside-exe save succeed. No version-based elevation. |
| SU-09 lock/saver | Passworded Alice locks from menu/secure attention, then passworded saver dismissal in normal and DOS/Win32 full-screen modes. Wrong password/Cancel/synthetic input and ordinary desktop crash preserve concealment; correct password restores the same session. Plaintext never appears in app input, clipboard, capture or serial log. Blank CiukiUser is told to set a password, not shown a false secure lock. |
| SU-10 resume | On a supported virtual standby provider, arm password-on-resume, suspend, wake; no application frame/input before trusted lock. Wrong password remains locked, correct password resumes. Unsupported provider is explicitly not run, not counted as a pass; physical confirmation remains outstanding. |
| SU-11 logoff | Open unsaved native/Win32 documents plus DOS VM. Veto leaves same session; consent closes all. Re-login Bob shows no Alice process, temp/hive handle or clipboard content. Switch User remains unavailable with reason. |
| SU-12 recovery accounts | Safe boot shows 500; test the lead-approved Q1/Q2 policy with correct/wrong/unset password, then normal enable/disable. Set-password recovery resets inactive Bob only through service; no passwordless recovery route. Disabled Guest never acts as fallback. |
| SU-13 crash/migration | Use runtime fault selection on disposable overlays: kill account service mid-management, leave stale v1 lock, interrupt each account/owner/migration publication stage, restart. Only a coherent old/new state or explicit closed recovery appears; no duplicate uid/admin, stale password revival after acknowledged reset, lost profile data or writable credential copy. |
| SU-14 shutdown | At Welcome choose Shut Down/Restart; fixed trusted power request succeeds with no desktop. Logged-in Bob's local shutdown follows save/veto; machine-policy edits still denied. |

### 10.2 Documentation and inventory migration

Keep G32's recorded implementation history and mark its profile-only security
rules superseded. Update seven requested inventory rows with the new acceptance
boundary, preserving earlier host evidence as historical evidence only:

| Row | Completion requires |
| --- | --- |
| SS-1 | Authenticated Welcome/logon, real uid/groups, private profiles and Win32 username/HKCU/CSIDL, SU-01..07. Network roaming remains out of scope. |
| SS-2 | Service-backed self password/reset policy and merged User Accounts; no default-profile/logon-disable bypass. Network password provider remains out of scope. |
| SS-3 | Kernel-backed trusted lock with saver path and full-screen revocation, SU-09; an opaque desktop overlay alone does not close it. |
| SS-4 | Authorized create/delete/type/password/picture and built-in enablement, last-admin/no-reuse rules, crash recovery. G32 CRUD alone does not close it. |
| SD-6 | Veto-aware teardown of the actual credential session, return to authenticated Welcome without auto-logon or Cancel escape, SU-11. |
| PW-8 | Existing lid/button behavior plus trusted password-on-resume ordering, SU-10, with qualified provider evidence explicitly identified. |
| DS-3 | Existing saver controls plus functioning per-user password setting and trusted dismissal hook; do not overwrite existing G10/DPMS limitations. |

Before integration, the lead adds explicit supersession pointers at affected
sections of execution, VFS, unified DOS, Win32, registry, POSIX, Setup, saver and
power contracts, updates SDK/user documentation and the relevant applet catalogue
aliases, and records the binding decisions through the lead's normal diary
process. This implementer does not edit the diary, source, SDK or test runners.

## 11. Open decisions for the lead

| Question | Recommendation / implementation blocked |
| --- | --- |
| Q1 — disabled recovery account in Safe Mode | Owner says 500 is disabled yet appears in Safe Mode. Permit a narrowly scoped Safe Mode logon with its configured password while retaining disabled status in normal boot. Visibility alone does not authorize authentication. Confirm identically in both halves before implementing recovery. |
| Q2 — recovery password on ready images | Setup never ran, so keep 500 unset and unauthenticatable until CiukiUser/another administrator sets a nonempty password; offer configuration in User Accounts. This means initial ready images have no configured recovery login. Confirm rather than distribute a shared recovery password or infer an empty-password exception. |
| Q3 — kernel/service interface reconciliation | Parallel kernel contract is absent in this worktree. Cross-review authenticated sender context, owner/account transaction commit, service-only registry storage, trusted input/display/secure attention, session teardown, UID sentinel/SYSTEM gid and Windows privilege-query mappings. No ABI numbers/layouts or implementation may be inferred here. |
| Q4 — first administrator on G32 upgrades | G32 has no trusted administrator to authorize an automatic promotion. Approve an explicit install/upgrade enrollment path with operator-selected first administrator, all others Standard and protected restartable mapping. Until then stop upgraded boots at enrollment-required; never promote all profiles or trust the first old password. |
| Q5 — unattended secrets and local password policy | Approve 8-63-byte nonempty passwords for custom users/500/Guest, only the directed CiukiUser blank exception, and schema-v2 fail-before-write behavior when secrets are missing. If fully unattended provisioning is required, choose a separate secret transport or reviewed verifier import; do not invent plaintext sample keys here. |
| Q6 — cloned ready-image machine SID | Local-only behavior can retain an image SID temporarily, but prefer entropy-backed per-install first-boot generation before account publication, protected and idempotent. Confirm with the kernel/database bootstrap and future networking before implementing; no network identity claim now. |
| Q7 — proposed policy details and release evidence | Approve the explicit stricter root policy, Guest password/removable-media limits, personal versus machine applet split, retained 10,000 KDF count pending real measurements and no persistent lockout. Retain directed Guest group 546 only despite XP's additional interactive Users membership. Qualify Pentium III latency; authorize the security QEMU suite under current policy. These are proposed CiukiOS choices, not assertions of exact XP defaults. |

The design deliverable is complete for review with those questions visible.
Runtime security, migration and installer qualification remain future work under
the reviewed implementation directives.
