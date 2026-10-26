# User security: accounts, logon, Setup and application compatibility

Directives: [sec-model](../directives/sec-model.md) and
[sec-contract reconciliation](../directives/sec-contract.md). Author: Codex.
Reconciled 2026-10-26 after running `date`; inspected worktree HEAD:
`7f31324b8f2099ee8ade98b3601b12e25857e058`.
Status: reconciled design for lead review; implementation and runtime
qualification are pending. [Lead decisions](../directives/sec-lead-decisions.md)
are binding. They are identified below as M-Q1..M-Q7 (model answers) and
K-Q1..K-Q8 (kernel answers), preserving the source's question numbers.

## Reconciliation

This document and [the kernel half](user-security-kernel.md) now specify one
policy. M-Q7 replaces this draft's read-only root with creator-owned top-level
files and folders: creator and administrators have full ordinary-data access,
other Users read/execute, Guest read-only. K-Q3 raises the bound to 256 such
entries per volume and applies the rule to additional fixed data volumes.
This is the lead's XP-inspired CiukiOS choice; Microsoft's XP baseline permits
root folder creation but root file creation is an explicit compatibility extension.

M-Q7's approved Guest limits resolve the removable-media disagreement in favor
of Guest read-only; Users may read/write. K-Q4 replaces the Win32-only all-System
write ban with the common prefix policy and refuses all user raw I/O on the
managed system disk. K-Q5 selects a user-session w32srv and a SYSTEM machine
registry broker. Authoritative hive backing files are service-only; native
personal preferences may use owner-writable files containing no authority.

M-Q3/K-Q1 are resolved by the credential, peer, transaction and trusted-gate
interfaces below and in the kernel half. SYSTEM has gid/group 0; active-account
role/disable/delete operations return EBUSY. G32 roots are explicitly mapped,
/Documents remains shared, and retired identities are never reassigned.
M-Q1/M-Q2/K-Q7 distinguish disabled, unset and password-authenticated Safe Mode
recovery. K-Q7 selects the Setup CD repair path if all administrator verifiers
are lost or corrupt, superseding this draft's reinstall-only recovery statement.

The kernel half freezes SYS_SECURITY=101 and its records; section
[Implementation plan](#implementation-plan) is the only directive/dependency
plan for both halves. No source, public header, test or image changes accompany
this reconciliation.

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

The coordinated kernel half is [user-security-kernel.md](user-security-kernel.md). It owns
credential records, syscall/IPC layouts and numbering, path resolution and owner
metadata, handle checks, process isolation, privileged operations and trusted
display/input transitions. This half specifies accounts and consumer behavior;
the frozen kernel ABI is defined there, without a second competing layout here.

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
| R5 | Microsoft [MS02-064](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064): XP root permissions include administrator/SYSTEM control, Users read/execute and directory-creation exceptions; upgrades may retain older permissive settings. | M-Q7 adopts creator ownership and additionally allows root files for old installers. It does not permit overwriting someone else's entries. Root file creation is a CiukiOS extension to the cited XP baseline. |
| R6 | Microsoft [Gaming with Least-Privileged User Accounts](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/gaming-with-least-privileged-user-accounts): XP NTFS protects Windows/Program Files; personal/shared data have writable locations; FAT32 does not supply that protection; limited-user HKLM access is read-only. | Apply an explicit kernel path policy and service-enforced registry policy. Compatibility must use permitted data locations. |
| R7 | Microsoft [registry key security/access rights](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights) and [predefined keys](https://learn.microsoft.com/en-us/windows/win32/sysinfo/predefined-keys): requested key access is checked; HKCU identifies the process user's HKU branch. | Scope handles to caller identity, bind HKCU at session creation, and protect backing files independently of Win32 APIs. |
| R8 | XP Resource Kit, [Authentication Policy Options](https://flylib.com/books/en/2.922.1.130/1/) (Microsoft-authored text, mirror): local account lockout is disabled by default (threshold 0); duration has no effect without a threshold. Password minimum defaults to zero; complexity is not enabled by default. | Choose no persistent lockout, with bounded retry throttling; stricter nonempty passwords below are CiukiOS policy. Current Windows lockout defaults are not substituted for XP. |
| R9 | Microsoft [Time](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/cc770579(v=ws.11)) explicitly applies to XP and requires administrative credentials to change time; [InitiateSystemShutdownEx](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-initiatesystemshutdownexw) distinguishes local/remote shutdown rights. Compaq's original [Installing Windows XP on Compaq Commercial Products, p.16](https://ftp.zx.net.nz/pub/mirror/ftp.compaq.com/pub/supportinformation/papers/15u1-1001a-wwen_rev1_us.pdf) (vendor paper mirrored from its FTP, indexed passage readable, full PDF fetch unavailable) documents Limited users unable to format Zip media and the administrative policy override. Microsoft [removable-media policy](https://learn.microsoft.com/fr-fr/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/devices-allowed-to-format-and-eject-removable-media) is a later reference. | Separate clock writes, local power requests, media data access, eject and raw formatting. Our format restriction follows the XP behavior; ordinary safe removal remains the explicit CiukiOS choice below. Do not present later client defaults as XP evidence. |
| R10 | Microsoft [logoff](https://learn.microsoft.com/en-us/windows/win32/shutdown/logging-off) and [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession). | Preserve G3 veto/save handling before destroying a session. |
| R11 | Microsoft [GetUserName](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamea), [OpenProcessToken](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocesstoken), [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation), [CheckTokenMembership](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-checktokenmembership), [IsUserAnAdmin](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-isuseranadmin). | Implement real read-only identity/token queries and documented buffer/error conventions, not unconditional success or a fixed administrator answer. |
| R12 | Microsoft [AllocateAndInitializeSid](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-allocateandinitializesid), [LookupAccountSid](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-lookupaccountsida) and [CSIDL](https://learn.microsoft.com/en-us/windows/win32/shell/csidl). | Supply bounded SID translation and profile-derived shell folders. |
| R13 | [RFC 8018 sections 4, 5.2](https://www.rfc-editor.org/rfc/rfc8018.html): PBKDF2 parameters include salt, PRF and iteration count. | Keep G32's algorithm and parameterized records; do not describe a fixed iteration count as a present-day security recommendation. |
| R14 | The Open Group [getpwnam/getpwnam_r](https://pubs.opengroup.org/onlinepubs/009695099/functions/getpwnam.html), [pwd.h](https://pubs.opengroup.org/onlinepubs/009604499/basedefs/pwd.h.html), [setuid](https://pubs.opengroup.org/onlinepubs/009604599/functions/setuid.html); upstream Linux man-pages [getuid/geteuid](https://www.man7.org/linux/man-pages/man2/getuid.2.html) corroborates the identity-query distinction. | Native libc queries use kernel IDs; public account lookup never exposes verifiers; credential-changing calls cannot elevate. Reconciliation could re-read getuid; getgroups/getpwnam/pwd.h/setuid retrieval failed (403 or inaccessible). These libc adapter semantics are explicit CiukiOS subset requirements, not a newly verified full POSIX-conformance claim. |
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
| CiukiAdministrator, uid 500 | Reserved built-in recovery administrator, groups 544+545. Cannot be deleted, renamed or converted to Standard. | Disabled and hidden in normal boot by default. Setup sets a password. M-Q1 permits password-authenticated Safe Mode logon while it stays disabled normally; an unset verifier still cannot authenticate (M-Q2). Explicit administrator enablement makes it available at normal logon. |
| Guest, uid 501 | Reserved, groups 546 only; cannot be deleted or made administrator. | Disabled by default. M-Q5 requires an administrator to set an 8-63-byte password before enablement. No failed/unknown-name fallback to Guest. |
| CiukiUser, uid 1000 | Only ready-made images that bypass Setup; groups 544+545. It is an ordinary account despite the distribution default name. | Enabled, passwordless, boot auto-logon as expressly required by the owner. |
| Setup custom user, uid 1000 | Mandatory on Setup installations, groups 544+545. CiukiUser is not also created. | Enabled with an 8-63-byte password (M-Q5). First ordinary session requires logon. |
| Later users, uid >=1001 | Administrator creates as Standard (545) by default or Administrator (544+545) explicitly. | Nonempty password required before enabling. Disabled records cannot log on. |

Account limits retain capacity for all 16 G32 users plus the two built-ins:
18 account records, excluding SYSTEM. Creating beyond the bound returns ENOSPC
without consuming an ID. Allocate monotonically within the serialized
creation transaction; once durably reserved, an id is consumed even if later
publication fails. Persist the allocation high-water mark across deletion;
deleted IDs/profile roots are never assigned to another account. ID exhaustion
refuses creation. UINT32_MAX is the invalid uid/gid sentinel, never allocated;
the last ordinary uid is 0xfffffffe. SYSTEM has primary gid 0 and group set [0],
and is absent from interactive account records. Administrators/Standard have
primary gid 545, Guest 546, consistently with the frozen ABI.

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

M-Q5 policy: 8-63 printable ASCII bytes (0x20-0x7e), case-sensitive, including spaces;
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
interactive verification is the common measurement target. A changed count needs recorded evidence
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

The eight-byte minimum follows the historical
[NIST SP 800-63B section 5.1.1.2](https://pages.nist.gov/800-63-3/sp800-63b.html#memsecretver)
cited by M-Q5. That revision was superseded in 2025 and recommended accepting
at least 64 characters. CiukiOS's 63-byte maximum is the approved format choice,
not a claim of current NIST compliance. K-Q6 keeps zero persistent lockout and
the one-second monotonic retry interval. Admit one active KDF and at most one
pending trusted-gate request; refuse excess attempts with EAGAIN and retry-after
bounded to 1000 ms. Never run KDF work in an input or filesystem lock.

### 3.3 v2 format and storage

The account service exclusively owns `/System/Security/accounts.db` on the system
volume, with `.new`, `.txn`, `.bak` siblings during replacement. All names,
backups, migration maps and journals are SYSTEM-only under the kernel policy;
FAT hidden/read-only bits and POSIX mode 0600 alone are insufficient. Public
name/picture/uid/group snapshots are a filtered service interface, not a
world-readable copy of this file. Profile ownership metadata is separately owned
by the kernel contract; this document does not define its encoding.

The serialized layout is little-endian, explicitly encoded, not a dump of C
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
accessible ownerless tree. The coordinated publication protocol in section
3.5 applies; the v2 transaction cannot pretend to provide cross-file atomicity
on its own. All I/O and KDF work runs off UI callbacks.

### 3.5 Account/owner commit and bootstrap handshake

The account service is the only account writer; the kernel owner-store service
is the only owner-map writer. Its private boot-installed channel accepts only
the sealed SYSTEM account/Setup service. It is not a user chown interface or a
second public credential syscall. Sec-k02 implements PREPARE, COMMIT, ABORT and
STATUS on this channel; sec-user-01 uses them. Each request carries transaction
id (the new accounts.db u64 generation), expected owner generation, uid, system
volume identity and resolved profile root. Replies carry the durable owner key,
owner generation and transaction state. Repeated identical operations are
idempotent; a reused id with different content or an unexpected generation fails
EAGAIN/EIO without rebinding an owner. QUERY_PEER authenticates both endpoints.

Creation/enrollment follows this order under the account service's live lease:

1. Reserve the monotonic uid and transaction id durably in the protected intent;
   after this point an aborted id is retired. Stage the profile with no ordinary
   traversal; reserve capacity against the 32-profile bound, including retained
   deleted profiles. No enabled account or desktop is published yet.
2. PREPARE validates the resolved root/volume and writes a durable pending owner
   record. Stage the new disabled account record and profile/hive defaults;
   journal old/new account and owner generations. No caller-supplied uid can
   acquire an existing unrelated tree.
3. COMMIT durably installs the owner binding and its policy generation. Only
   after acknowledgement install the enabled accounts.db generation and mark
   the cross-store intent committed, fsync and acknowledge account creation.
   The frozen session profile descriptor names that owner key/generation;
   SESSION_CREATE refuses pending, missing or mismatched ownership.
4. Recover before accepting clients after a crash. A committed owner with no
   published account remains inaccessible to ordinary users and consumes its
   reserved uid; complete the intended account or retain disabled evidence.
   Never roll back an acknowledged password change or reuse an orphan's uid.
   ABORT cancels only pending publication, never erases retired-id evidence.

Deletion, disablement and group changes of the active account return EBUSY
before any persistent change; request Log Off and retry. The kernel still
supports SYSTEM end-session for crashes/recovery, and immutable credentials
are never edited in place. Deletion retires the uid and owner binding; retained
files remain owned by that retired uid. Rename only changes the label. Password
and picture changes may update the active account without replacing its token.
The 18-account limit and 32-profile capacity are separate; retained data can
reach the profile bound before the account limit, returning ENOSPC safely.

PID 1 starts only the sealed SYSTEM service set in session 0. Before any
account publication, the account service recovers/creates the per-install
machine SID and validates account/owner stores. A ready-image manifest is an
explicit unprovisioned seed, consumed once through a protected intent; a missing
store on an already provisioned install never replays the seed. Setup uses the
same handshake against its destination volume, not the source running session.
G32 upgrades stop at the trusted enrollment gate until conversion commits.
Setup stages verifiers/owners rather than publishing accounts on the destination;
its installed first boot generates the SID before promoting the seed. The seed
uses the section 3.3 encoding with magic `CIUKISEE`, version 2, zero machine-SID
components, origin 1 or 2, the same bounded records/checksum, and no account
publication semantics. Store it as /System/Security/provision.seed. Only the
unprovisioned bootstrap state accepts it; ready seeds describe blank CiukiUser
but contain no shared verifier, so first boot generates its salt/verifier.
Setup seeds carry the entered passwords' salted verifiers. First boot validates
the seed, generates/persists SID, re-encodes the ordinary CIUKISEC database,
commits owner/account state, and marks the seed consumed before admitting a
session. Recovery uses a protected intent and never replays a consumed seed
if accounts.db is subsequently lost. Copying a ready seed into a Setup payload
is refused. Neither seed nor accounts.db is publicly readable.
After authentication, profile/hive validation and SESSION_CREATE, launch only
the selected uid's desktop and w32srv. Kernel ACTIVE means the session was
admitted; the UI reaches ACTIVE only after the fresh desktop is ready. Failed
launch closes that session and retains the trusted error/logon screen.

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

Account-service UI states are UNPROVISIONED, LOGON, AUTHENTICATING, ACTIVE,
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
to reach the trusted UI. Ctrl+Alt+Delete is consumed by the kernel from trusted
physical keyboard events before ordinary delivery; it opens the SYSTEM security screen. Software
messages/guest input cannot generate it. Sec-k07 implements and qualifies this
transition for every input backend; until then no secure-lock claim is made.

Unlock requires the current session owner's password. No arbitrary administrator
can unlock into another user's desktop in this step; an administrator can
request orderly/confirmed session termination through the trusted UI, then log on afresh. Authorization is verified by the SYSTEM
service; it never unlocks into the other user's desktop.
Cancel or failure remains locked. Application state stays alive on a lock;
notifications/painting/input remain concealed. A crashed ordinary desktop does
not make the trusted lock disappear. Account-service restart invalidates stale
authentication replies and preserves the locked state.

Passwordless CiukiUser cannot provide a password-protected lock. Show "Set a
password to lock this account" and open its password-change path; do not present
an empty-password gate as security. Savers can still conceal the screen. Once a
password exists, saver password and resume password are enabled by default;
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
CiukiAdministrator password/confirmation. Both passwords require 8-63 printable
ASCII bytes (M-Q5). The owner mandated a custom administrator and choosing the
recovery password; the approved nonempty-password policy makes the authentication boundary
meaningful. M-Q5 fixes the minimum without claiming XP default parity. Reject
reserved names, including CiukiUser;
Setup installs never create that distribution-default account.

Extend screen 7 or add a keyboard-accessible account page with the same validated
answers model. Review pages show only names and "Password set", never passwords,
verifiers or salts. Do not save secrets into System\\Identity, SETUP.STATE,
SETUP.LOG, manifest summaries, serial output or BOOT.CFG. Keep account
data in bounded worker-owned buffers and erase it on Back/Cancel/finish/error.
Collecting or cancelling still performs no ATA writes before the existing
Install confirmation.

During libsetup's configuration phase, trusted boot-only Setup stages uid 500
(disabled, password configured), uid 501 (disabled, unset), uid 1000 (custom
administrator), owner metadata, profile directories and registry defaults on the
destination prefix. It writes a SYSTEM-only provisioning seed containing salted
verifiers, never plaintext or a published accounts.db. Destination seed/owner
integrity is checked before sealing. On the first installed boot, SYSTEM obtains
the per-install machine SID from entropy, then commits enabled accounts.db and
owner publication before logon (M-Q6). It must not modify source-media or
host-session accounts. Configuration retries use the same reserved uid; a failed
seed/credential commit cannot boot an ordinary desktop.

After installed-disk boot, SYSTEM logon runs before the existing firstboot
wizard. The authenticated administrator's desktop runs the wizard without SYSTEM
credentials; privileged machine actions go through authorized services. The CD
remains Setup-only; generic rescue stays read-only and the explicit confirmed
account-repair path in section 5.3 is the K-Q7 exception.

### 5.2 Unattended installation and images

Use SETUP.INF schema version 2 for account-complete answers. Version 1 files with only UserName are no
longer complete account answers. Retain disk/component/network rules, require
UserName and both passwords. The initial safe implementation can prompt for the
two passwords when no secure answer transport exists; explicitly unattended mode
then stops with `status=missing_answer` before any disk write. Do not silently
use blank defaults or place plaintext secrets in a distributed sample answer
file. M-Q5 defers secret transport/verifier import. This step prompts interactively
or refuses unattended installation before writes; it defines no plaintext
password INF keys or distributed secrets.

`scripts/build_image.py` must distinguish ready images from Setup provisioning
templates using explicit build metadata, not absence of users.dat. Ready images
carry enabled passwordless CiukiUser uid 1000 plus disabled reserved accounts.
CiukiAdministrator initially has an unset verifier on such images: its password
was never chosen in Setup. It cannot authenticate even in Safe Mode until an
authenticated administrator sets it. The account applet offers that configuration
after setting CiukiUser's password. This is binding M-Q2/K-Q7 policy; no distributed
known password or secret hidden in a public image is acceptable.

A Setup payload must not accidentally copy the ready-image auto-logon/account
database onto the destination. Shared payload generation, Setup catalogues and
static image checks must recognize the provisioning distinction while retaining
one canonical runtime build and the approved Ciuki identity/assets.

### 5.3 Lost passwords

If a normal administrator password is lost, Safe Mode shows CiukiAdministrator;
after verification with its configured password, it may reset an inactive normal
account. Ordinary Safe Mode does not enable Guest or skip authentication. M-Q1
permits this narrow password-authenticated exception for normally disabled uid
500. If every administrator verifier/password is lost or corrupt, K-Q7 requires
the Setup CD repair path, under physical control, with explicit destination
selection, backup warning and confirmation before any write. Repair sets a new
8-63-byte recovery password and repairs account/owner transaction state; it does
not start an unauthenticated installed desktop, reuse a retired uid, or silently
reassign private data. If integrity cannot be established, keep the volume closed
and offer backup/reinstall. Repair is restartable and redacts secrets. Its
bounded account-repair operation is implemented in sec-user-02; generic rescue
log collection stays read-only. This explicit repair exception supersedes the
older CD plan's blanket ATA read-only rescue statement for this operation only.
It is not an online empty-password recovery route.

## 6. Profiles, migration and protected storage policy

### 6.1 Paths and permissions consumed from the kernel half

Keep numeric roots `/Windows/Profiles/u%08x` to avoid a bulk data move and
username/path ambiguity. New roots use the new uid; imported roots can retain
their old numeric suffix through protected owner mapping. There is exactly one
canonical root per uid, looked up through the service/kernel metadata. The
suffix or a client-provided HOME is never proof of ownership.

| Managed system-volume tree | SYSTEM / administrator | Standard / Guest |
| --- | --- | --- |
| Volume root (every fixed managed volume) | SYSTEM and administrators manage ordinary entries; sealed anchors remain SYSTEM-only. | Standard may create top-level files/folders, owner full control, other Users read/execute, Guest read-only. No modification/deletion of another owner's entry (M-Q7/K-Q3). |
| `/System/Security` and every account/owner journal | SYSTEM only. Administrator uses validated service requests. | No read, enumerate or write. |
| `/System`, `/Programs`, `/Windows` including SYSTEM/SYSTEM32, fonts and machine settings | SYSTEM services and authorized administrator maintenance; the common prefix policy replaces the extra Win32 all-/System write ban; F5 builtin-store restrictions remain (K-Q4). | Read/execute public payload; no machine mutation. More-specific private profile rules override public read. |
| `/Program Files` including Common Files | Administrator installs/updates; system DLL replacement still obeys F5 builtin rules. | Read/execute, no write/create/delete. |
| `/Windows/Profiles/<mapped root>` | SYSTEM; administrators may manage user files through policy-authorized operations. | Owner has read/write/create/delete in ordinary profile data; other Users/Guest denied; Guests only their own Guest root. Protect the profile root/owner binding against reassignment. |
| `/Windows/Profiles/Default` | SYSTEM/administrator manage template. | Read-only template, no active identity. |
| `/Documents` and `/Documents/Desktop` after migration | Becomes an explicitly shared legacy data tree; administrator manages its publication. | Users read/write shared documents; Guest read-only. It is not everybody's private home. |
| Global Start Menu/Desktop defaults and `/System/Registry` | SYSTEM/administrator updates machine configuration. | Read; personal overrides go under the profile. Never let a shared shortcut/data directory inject SYSTEM startup code. |
| Per-profile Documents/Desktop, Favorites, Start Menu/Programs, Application Data, Local Settings/Temp, settings | SYSTEM/service and owning user. | Owner can edit; other accounts denied. Credential/owner records never reside here. |

This is the required user-visible matrix, not a redefinition of kernel resolver
algorithms. All native/Win32/DOS routes, 8.3 aliases, case variants, volume aliases,
relative paths, `..`, moves and inherited handles must obey the kernel half.
Source and destination authorization both matter on rename. Raw volume/device
access must not let a user edit protected files underneath the policy. Unmapped
private roots or unavailable owner metadata are denied, not treated as shared.

Removable FAT media are explicitly shared transport, not per-user private
storage: Users and administrators may read/write files; Guest is
read-only (M-Q7). Ordinary eject/safe-remove requests use the existing busy/flush checks;
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

K-Q5 requires one w32srv under the immutable uid/groups/session of its user;
window, clipboard and user-object work never runs as SYSTEM. A separate sealed
SYSTEM machine-registry broker owns machine hives and their persistence. It
also performs narrowly scoped persistence of authoritative personal hives, so
no raw hive fd needs to be passed into a user process. Machine backing paths are
`/System/Security/Registry/Machine`; private authoritative hive slots, intents
and backups are under `<profile>/Settings/Registry`, a SYSTEM-only anchored
subtree. The kernel prevents renaming/deleting that subtree or its ancestors.
Ordinary native Desktop/application preference files outside it remain
owner-writable via libreg, contain only personal preferences, and cannot set
roles, machine policy or HKLM. Native machine-registry changes use the SYSTEM
broker and administrator checks; backing files remain service-only.

The session w32srv accepts only kernel-retained same-session peers. Logical
HKCU operations use that peer's uid/session; persistence is bound to the
registered w32srv's identical credential and fixed profile, never a supplied
hive path. Machine broker requests use direct authenticated client links, or
the registered session server with the same immutable credential; a forwarded
uid/pid string is never authority. If a request requires original per-process
rights, it must use the direct retained link, not unverifiable forwarded bytes.
Broker ACL/key checks apply on every operation and deferred worker request.
`w32srv` must enforce key access on the authenticated caller. Registry handles retain uid/session/access scope and are
invalid after teardown; another process cannot adopt a raw numeric handle.
The storage worker serializes writes per hive/root and releases dead-client
leases, addressing the native registry's current single-writer requirement.
Direct backing-file access must not bypass it: authoritative hives and their
intent/backup files need service-only subdirectories, even within an otherwise
user-writable profile. Applications receive authoritative HKCU and machine-registry operations through
services, not writable raw hive files. Personal non-authoritative libreg files
remain subject to ordinary owner policy. A writable parent must not let the owner
rename, remove or relocate a service-only hive directory to bypass its policy;
that is a required kernel-half subtree-protection case. Existing personal non-registry settings
can remain owner-writable where generation-safe concurrent access is supported.

### 6.3 G32 conversion

Migration is one SYSTEM-controlled, restartable transaction before an ordinary
session starts. Version 1 G32 records carry no security roles and were stored on
a shared filesystem; possession of one old password is not authority to promote
that user or to approve machine migration.

M-Q4 requires an explicitly initiated upgrade/enrollment workflow. Require
the operator to choose the first custom administrator and recovery password;
assign the selected administrator uid 1000. Import every other G32 named profile
as Standard, assigning fresh uids in old-id order. If an old name collides with a
reserved name, require a replacement label while retaining its files; never
reinterpret an old SYSTEM/Guest/CiukiUser label as a built-in account. Persist
old-id -> new-uid -> existing-root mapping, protect owners before enabling, and
do not recycle the old profile directory. Import at most the existing 16 named
users without truncation. If there are none, require creation of a new custom
administrator; absence of G32 data is not a ready-image signal.
Resolve numeric-directory collisions before publication: existing roots retain
their explicit owner mapping, never an inferred new uid. If enrollment needs
that occupied spelling for a newly created first profile, stage and relocate
the legacy root to an unused numeric root through the same protected journal,
then commit its mapped owner. Later allocation skips/consumes ids whose fresh
numeric path is already reserved by another retained tree; no overwrite or
silent ownership transfer is permitted.

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
account and change its own password with old-password verification, but cannot alter type,
enablement or other accounts. Re-check authorization when applying, not only
when opening a page. All unavailable actions have visible explanations.

| Applet/page/action | Standard/Guest view | Administrator action |
| --- | --- | --- |
| Device Manager / drivers / hardware enable-disable | Public inventory, read-only; modification disabled. | Authorized machine changes; trusted driver-install/update path only. |
| Network adapter/IP/DNS/DHCP configuration, global proxy/shares/lobby defaults | Status readable; machine edits and DHCP release/renew disabled. Per-user application networking continues. | Machine configuration and DHCP control; no new network-authentication provider. |
| Date/time and system time zone/RTC | Read clock, personal formatting/locale editable; clock/time-zone writes disabled. | Change system time/time zone through checked service. No Power Users tier. |
| Power | Battery/status and personal monitor/saver/resume preference controls usable; machine standby/disk timers, button/lid defaults disabled. | Change machine policy. Users/Guest may request local power actions via the session UI. |
| Display | Personal wallpaper/colors/theme/saver editable; read current mode. Supported session mode/refresh/DPMS and game leases allowed through the compositor; driver and machine-default changes disabled. | Change machine defaults/driver configuration through the trusted path. Session display choices persist only to that user. |
| Add/Remove Programs | Own-profile install/uninstall and published program list; all-users actions disabled with reason. | All-users install/uninstall and bundled program management. |
| Sounds, keyboard/mouse, regional, accessibility | Personal preferences editable; public device capability view. Machine driver/default-page changes disabled. | Machine defaults/device settings as applicable. |
| Registry Editor, startup recovery, ScanDisk, disk tools | Personal keys/files editable through scoped services; machine edits/repair/raw writes disabled. Public nonsecret reports allowed; volume-wide ScanDisk/defrag analysis requires an administrator and a SYSTEM maintenance service on the system disk. | Checked machine edits and repair; SYSTEM security records still service-only. |

These are approved CiukiOS rights informed by XP (M-Q7/K-Q8), not a byte-for-byte clone of
every XP applet default. When a control combines personal and machine fields,
split its save request and storage scope; opening Control Panel never elevates
the whole process. Authentication/policy failures must be observable in direct
service and syscall tests even when the UI is bypassed.

### 7.2 Authority conversion inventory

| Existing authority/input | Mapping to the new model |
| --- | --- |
| PID 1/bootstrap, protected logon/account service | SYSTEM-only. Arbitrary credential creation, account-store I/O, trusted gate and account/owner publication stay here. Administrator membership does not grant uid 0. |
| `dosvm_capability`, `/Programs/dosbox` exact-path launch | Per-user VM creation; uid/session inherited, all guest I/O checked for that user. Existing binary/grant restrictions can remain as implementation-resource checks, never role grants. |
| `programs_capability`, bundled-program GAME_REMOVE/RESET/EXPORT | Machine game removal admin-only; own save query/reset/export per-user, with destination policy checks. No implicit all-users save reset. Audit each operation instead of elevating every DOS action. |
| `network_capability`, `dhcp_capability`, corresponding AUTHORITY/status bits | Machine network and DHCP mutation admin-only; status and ordinary sockets per-user. Existing trusted DHCP boot service remains SYSTEM with narrow internal work. |
| `time_capability`, `CIUKI_TIME_AUTHORITY` | System time/time zone admin-only; public time and personal formatting per-user. Status bit reflects actual rights, not binary name. |
| `recovery_capability`, `CIUKI_RECOVERY_AUTHORITY` | Boot/startup mutation and repair admin-only; bounded nonsecret reports per-user; every whole-volume scan/repair admin-only through SYSTEM on the system disk; protected boot-service work SYSTEM-only. |
| `setup_capability`, Setup mode and destination-disk lease | SYSTEM-only boot Setup, fixed destination and existing no-write-before-Install rule. A normal administrator launching setup cannot acquire this privilege by pathname. |
| POWER authorization from display/input grants | Per-user request of local power actions through session broker; machine policy admin-only; logon-screen fixed shutdown/restart SYSTEM-only UI. Possession of grants does not confer all power-policy rights. |
| `CIUKI_TASKS_FD`, `--authority-fd`, call 71 and signal/termination routes | Per-user termination within that uid/session with existing self/desktop/controller restrictions; admin may request ending another ordinary session/process under kernel policy. SYSTEM/PID 1/protected services remain protected. An inherited fd or forged argument cannot cross the role boundary. |
| Display/input grants, delegated Win32 presentation and master-mixer fds | Per-user/session grants; revoked/scoped by trusted lock/session transitions. Mixer access for the active session does not confer account, driver or machine-policy rights. |
| Win32 service/loader/compatibility-writer registration and trusted paths | SYSTEM registers trusted services; each request uses the caller's identity. App loading/extraction is per-user; machine shim database/admin configuration admin-only. Existing service endpoints must not turn a user-writable binary/path into authority. |
| Environment (`CIUKI_PROFILE`, HOME, USER, TMPDIR, CIUKI_* fd strings) | Per-user hints only; consult kernel credentials for authority and protected metadata for canonical profile binding. |

Keep legacy constants/SDK signatures where compatibility requires them; their
reported truth and effective scope must follow the new policy. Do not maintain an
independent path-derived administrator mechanism alongside credentials. The
kernel half owns the enforcement inventory and the single SYS_SECURITY=101
reservation; consumers use its frozen definitions.

## 8. Win32, native and DOS interfaces

### 8.1 Win32 identity and token subset

Translate local uid u to `S-1-5-21-A-B-C-u`, where A/B/C are installation-stable
machine components, except SYSTEM -> `S-1-5-18`. Groups map to
`S-1-5-32-544/545/546`. Local lookup returns the configured computer name as
account domain, BUILTIN for built-in groups and NT AUTHORITY for SYSTEM.
Computer rename changes the display domain, not persisted SIDs. M-Q6 requires
entropy-backed A/B/C generation on first boot before any account publication, including cloned ready images; persist it atomically and idempotently
in protected bootstrap state before copying it into accounts.db. Existing
provisioned machines retain it across updates. Entropy failure prevents logon;
no build-time shared SID or clock fallback. There is no network SID trust here.

| API subset | Required behavior and limits |
| --- | --- |
| GetUserNameA/W | Authenticated process username, not `Ciuki`, USER/HOME or a desktop-selected string. Buffer size includes terminator; insufficient buffer reports required size and ERROR_INSUFFICIENT_BUFFER. No impersonation in this step. |
| OpenProcessToken | Obtain query-only handle for own process; other-process access requires the kernel's process-query authorization. Store uid/session scope in the real handle table. TOKEN_QUERY supported; adjustment/assignment/elevation rights refused. CloseHandle releases it. |
| GetTokenInformation | TokenUser, TokenGroups, TokenPrimaryGroup, TokenOwner, TokenType, TokenSessionId and TokenPrivileges from the kernel credential/operation-right view. Sized buffer queries, self-contained SID/pointer layouts and ERROR_INSUFFICIENT_BUFFER; unknown classes ERROR_INVALID_PARAMETER. No UAC TokenElevation story is fabricated. |
| CheckTokenMembership / IsUserAnAdmin | NULL token checks current effective identity. Administrators group is true only for 544, Users for 545, Guests for 546; no automatic admin true for every program. IsUserAnAdmin is a membership wrapper. R11 requires an impersonation token for a non-NULL CheckTokenMembership handle: return ERROR_BAD_TOKEN_TYPE for an explicit primary-token handle. DuplicateToken remains outside this step; an installer needing explicit impersonation tokens must receive a typed unsupported/error result, not a fabricated credential. |
| AllocateAndInitializeSid / FreeSid / IsValidSid / EqualSid / GetLengthSid and accessors | Retain/complete validated NT-layout data helpers. AllocateAndInitializeSid accepts at most eight subauthorities and never creates an account or privilege. Fail invalid SID/length/arithmetic without overwriting buffers. |
| LookupAccountSidA/W | Local SYSTEM, built-ins and current stored accounts only; correct name/domain sizing and SID_NAME_USE. Unknown SID ERROR_NONE_MAPPED; no network lookup. |
| LookupPrivilegeValue and TokenPrivileges | Exactly the kernel half's three local LUIDs: shutdown=1 for interactive accounts, system-time=2 and managed driver requests=3 for administrators; SYSTEM has all three. Granted entries have attributes 3; absent entries are omitted. No SeDebug/SeTcb/SeAssignPrimaryToken. Adjustment fails explicitly and cannot create rights. |

The exact read-only TokenPrivileges mapping is frozen in the kernel half;
section 8.1 consumes those names, local LUIDs, attributes and refusals unchanged.
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
does not alter credentials. K-Q4 replaces the old blanket `C:\\System` write denial with the common
prefix policy; sealed boot/service inputs and the F5 builtin store still require
the trusted maintenance path. Standard users may also install into a newly
creator-owned root folder such as `C:\\GAME`; other Users receive read/execute
and Guest read-only. This route is shared-readable, unlike a private profile.

### 8.4 Native libc and DOS

Provide getuid/geteuid with equal 32-bit values from immutable credentials;
getgid/getegid give Users (545) for ordinary administrators/Standard, Guests (546)
for Guest, and SYSTEM primary gid/group 0, as frozen in the kernel half. getgroups returns actual supplementary membership including 544 for
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

## Implementation plan

This is the single ordered plan for both halves (sec-contract item 4). IDs
sec-k01..k08 and sec-user-01..06 are stable; the kernel half does not maintain
another sequence. Dependencies are integration prerequisites, not permission
to claim security after a partial merge. The lead writes each directive from
these frozen requirements, cross-reviews, integrates and records evidence.
No host test, QEMU run or build is part of this documentation task.

Independent work may use separate worktrees once sec-k01's common ABI and
retained-subject interfaces are integrated. Immediately after it, sec-k02 and
sec-user-01 can run concurrently: the latter uses the specified owner-service
adapter and fault-injecting backend, but cannot publish real accounts until
sec-k02 is integrated. Later waves below identify disjoint production areas.
Shared runner/header registration is serialized by the lead; implementations
must not concurrently rewrite common syscall tables, headers or test scripts.
Parallel implementation never permits parallel host suites/heavy jobs.

| Order / directive | Dependencies | Files / areas owned | Acceptance criteria and host evidence |
| --- | --- | --- | --- |
| 1. **sec-k01 — credential and session foundation** | This reconciled freeze | `src/kernel/include/ciuki/{abi,process}.h`, `proc/{process,spawn,supervisor,syscalls_proc}.c`, common syscall dispatch, minimal Win32 child inheritance, channel/service retained-subject primitives, libciuki/newlib query wrappers, ABI oracle/SDK copy | Immutable credentials before process/thread publication; seven call-101 operations and exact layouts/constants/errors; gid 0, invalid uid sentinel and monotonic session ids; public spawn cannot elevate; protected service bootstrap and cross-credential fd sanitization. Allocation/copy failure exposes no child/session; peer fd reuse/forged uid fails. **H01**, ABI oracle and API-reference regeneration; existing kernel process/desktop regressions. |
| 2A. **sec-k02 — managed-volume policy and owner service** | sec-k01 | `src/kernel/fs/` policy/owner store and provisioning channel, resolved-object labels, `proc/posixpath.c` adapter; keep broad syscall enforcement for k03 | 32 rules/32 profiles/256 creator-owned entries per volume; Standard creates root files/folders with owner control and other-user read/execute; Guest read-only; aliases/LFN/8.3/dotdot/mount generations agree. Explicit owner PREPARE/COMMIT/ABORT/STATUS and denied-first account handshake; every power cut and capacity failure preserves ownership/retired ids. Warm label lookup bounded. **H02**, **RFS**. Concurrent with sec-user-01. |
| 2B. **sec-user-01 — SYSTEM account service/store** | sec-k01; real publication additionally requires sec-k02 | New sealed account/logon service, private account IPC, v2 codec/KDF/filtering, `apps/desktop/users*` migration adapter; no ordinary desktop gate yet | 18-account schema, entropy-backed machine SID bootstrap, 8-63-byte policy/sole ready blank exception, 10,000 KDF count, one KDF + one pending request/1000 ms retry; valid identity/last-admin/active EBUSY rules; durable reservation, stale generation/crash-lock recovery and no acknowledged-password rollback. Tests use the k02 owner adapter; service refuses live publication until it is available. **H03**, **RUS**; root/owner integration after k02. Concurrent with k02, no writes to its kernel files. |
| 3A. **sec-k03 — all file/handle/mapping enforcement** | sec-k02 | `proc/{syscalls_file,fdtable,posixpath}.c`, VFS read/write/metadata paths and workers; Win32 file/section/view/loader sources only | Every native and lower-level entry point checks retained subject; cached exec, truncation, O_TRUNC, rename endpoints, root-owner rename/delete, directory slot reuse, attrs/times, mappings and deferred delete cannot bypass policy. Denied spy backend issues no private reads/writes. Cross-root move fences/revokes or returns EBUSY unchanged. **H04**, **RFS**, **RK**. Concurrent with k04 after k02; device dispatch edits belong to k04 and are integrated serially. |
| 3B. **sec-k04 — device and administrative authority** | sec-k02 | `drivers/{disk_dev,media_dev}`, `fs/disklease`, media/recovery/time/network/power and device dispatch; no generic file-policy edits | Every inventory action classified; remove executable-path role grants. Deny admin raw reads/writes on managed system disk; SYSTEM maintenance after admin check; retain exclusive leases/fencing. Guest read-only removable, local power allowed, session input/mixer/display distinct from machine defaults. Audit INPUT_KEYBOARD_SET and CIUKI_SETUP_NET_QUERY production dispatch; reject unsupported routes until repaired. **H05**, **RNET**, **RK**. Concurrent with k03, then user-02 may start. |
| 4A. **sec-k05 — DOS and per-user saves** | sec-k03, sec-k04 | `src/kernel/vm/dos*`, DOSVM client/save/provider paths, DOS launcher adapters | VM/workers inherit immutable uid/session; INT21 handle/FCB/LFN/EXEC/metadata paths enforce VFS denial (DOS 5, no local-FAT fallback). Forged DFV2/PSP/drive/LBA and INT13/25/26 never reach a physical-disk callback. Separate COW/saves/temp/print/clipboard per user; own reset/export and admin machine uninstall. No memory cap change. **H06**, **RK**. Concurrent with k06/user-02 in separate files. |
| 4B. **sec-k06 — Win32 isolation and registry broker** | sec-k03 | `proc/win32/` object/service/token/message/APC/snapshot/diagnostics paths (file/view/loader changes from k03 already integrated); `apps/w32srv` persistence/registry and new SYSTEM broker, native machine registry service | Real query-token handles and kernel credential facade, one user w32srv/session, SYSTEM-only raw authoritative hives; HKCU/HKU/.DEFAULT and HKLM checks on every operation including imports/queues. Cross-session objects, handles, APCs/messages/pixels/crash data blocked; self-memory restrictions preserved. Direct peer or identical registered session subject only, no forged forwarding. **H07**, **RW32**, **RK**. Concurrent with k05/user-02. |
| 4C. **sec-user-02 — Setup, ready images, migration and repair** | sec-user-01, sec-k02, sec-k04 | `apps/setup/`, `sdk/libsetup/`, `scripts/build_image.py`/shared Setup payload metadata, firstboot adapter, enrollment and confirmed CD repair | Custom uid 1000 and passworded disabled 500 on Setup, no CiukiUser; ready seed creates blank CiukiUser once and leaves 500 unset. Generate machine SID before publication; no seed replay after loss. Unattended missing secrets fail before writes; no secret transport in scope. Restartable explicit G32 enrollment/maps, shared /Documents preserved, old verifiers protected. Confirmed lost-password CD repair is bounded and restartable. No pre-Install ATA writes or plaintext log/state output. **H08**, **RSET**, **RLS**, **RUS**. Concurrent with k05/k06; builder changes are source work only in this later directive. |
| 5. **sec-k07 — trusted gate and kernel teardown integration** | sec-k03, sec-k04, sec-k05, sec-k06, sec-user-01 | Kernel secure-attention/input/display/grants and session drain; supervisor/session service integration | Kernel consumes trusted physical Ctrl+Alt+Delete; secure UI revokes user input/fullscreen/framebuffer access including mapped and accelerated paths. Logoff blocks fresh work, stops all orphans/threads/Win32/DOS, drains admitted I/O, revokes handles/views/clipboard/replies. Failure to fence blocks next logon without panic. Session 0 survives; stale authentication/service replies cannot resurrect a desktop. **H09**, **RK**, **RWM**. No changes to ordinary logon/app UX owned by user-03. |
| 6A. **sec-user-03 — authenticated logon and session UX** | sec-k07, sec-user-02 | Trusted account-service UI, `apps/desktop/logon.inc`, profile/session startup and veto-aware Log Off | Enabled Welcome/typed names only, Safe Mode 500 password exception, unset 500 refusal, disabled Guest refusal; Cancel/Close never starts desktop. Ready autologon only valid tuple and never after Log Off/Lock/Safe Mode. No SYSTEM fd/environment in desktop; profile/hive/startup failure retains secure error UI. Veto retains identity; unlock restores same session; passwordless CiukiUser gets set-password guidance. **H10**, **RUS**, **RWM**. Concurrent with user-05 after shared service interfaces are integrated. |
| 6B. **sec-user-05 — application-facing identity and compatibility** | sec-k05, sec-k06, sec-user-01, sec-user-02 | `sdk/win32/advapi32`/shell token, SID, username, CSIDL and privilege facades; native account lookup/libc adapters; DOS launcher profile consumers | Exact frozen read-only token/privilege view, SID/name short-buffer errors and truthful groups; direct authoritative kernel queries, no authority from client SIDs. HKCU, HOME/TEMP/CSIDL and per-user saves agree; original installer metadata cannot elevate. Standard profile or creator-root install succeeds when supported, protected all-users install denies; F5 builtin protections persist. **H11**, **RW32**, **RUS**, relevant native ABI oracle if public headers change. Concurrent with user-03, no edits to its gate. |
| 7. **sec-user-04 — Control Panel, saver and resume** | sec-user-03, sec-user-05 | `apps/control/users.inc` and catalogue aliases/machine pages, desktop personal settings, saver/power hooks | User Accounts merges Users/Passwords with old launch alias, truthful admin/Standard/Guest controls; apply-time broker checks; last-admin/active EBUSY rules; session/machine saves separated. Guest self-password follows same old-password rule. Trusted saver dismissal and resume lock arm before restoring display/input; preserve unsupported standby/DPMS limits and exact Ciuki assets. **H12**, **RUS**, **RWM**, existing saver/power/control harnesses. |
| 8. **sec-k08 — coverage and bounded-cost audit** | sec-k01..sec-k07, sec-user-02, sec-user-04, sec-user-05 | Source-derived syscall/action policy census, cross-frontend adversarial harness and performance/ledger validation document | Every published selector has one policy/test mapping; unknown selectors deny. Warm fd checks add no storage lookup; measure metadata at 32 profiles/256 roots per volume, group/session/process limits and every final-reference failure. No secret/fd/pin/page/VM leak after successful teardown. KDF PIII/T23 measurements remain a lead runtime gate. **H13**, **RK**, **RFS**, **RW32**, complete security host harness. |
| 9. **sec-user-06 — integration and runtime evidence preparation** | sec-k08, sec-user-01..sec-user-05 | Documentation/inventory, runner-only security scenarios and private-fixture adapters | Wire SU-01..14 plus root-owner/Guest-removable/CD-repair cases below as runtime cases without build variants. Record all host commands/results; reconcile API docs and supersession pointers. Inventory stays partial until its evidence exists. **H14** (host parser/fixture wiring only), all relevant regressions serially. Canonical build, authorized QEMU and hardware are the lead's later work. |

### Host acceptance commands (future implementation only)

H01..H14 below are **new harnesses to create**, not current runnable tests or
passes. Each imports/links the actual production entry points named in its row,
with hardware/storage fakes only at the boundary and independent expected
results. A duplicate policy model is not enforcement evidence. Each directive
wires its harness into the relevant existing runner, keeps ASan/UBSan and leak
checks, and records exact commands/results. The H03 store harness must retain
leak detection as the existing G32 users harness does; no global sanitizer
exception may be added. Run one command at a time, each at nice 19.

| Id | Planned exact command |
| --- | --- |
| H01 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_credentials.py` |
| H02 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_owner_policy.py` |
| H03 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_accounts.py` |
| H04 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_file_enforcement.py` |
| H05 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_devices.py` |
| H06 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_dos_identity.py` |
| H07 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_win32_broker.py` |
| H08 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_provisioning.py` |
| H09 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_secure_gate.py` |
| H10 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_logon.py` |
| H11 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_identity_facades.py` |
| H12 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_account_controls.py` |
| H13 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_policy_coverage.py` |
| H14 | `nice -n 19 python3 -B -m unittest discover -s tests/host/security -p test_runner_contract.py` |

These existing entry points are regression inputs, not claims that current
assertions cover future security:

| Id | Existing command |
| --- | --- |
| RUS | `nice -n 19 python3 -B -m unittest tests.host.desktop.test_users` |
| RWM | `nice -n 19 python3 -B -m unittest tests.host.desktop.test_window_model` |
| RW32 | `nice -n 19 python3 -B -m unittest discover -s tests/host/win32 -p test_registry.py` |
| RSET | `nice -n 19 python3 -B tests/host/setup/run.py` |
| RLS | `nice -n 19 bash scripts/test/host_libsetup_tests.sh` |
| RK | `nice -n 19 bash scripts/test/host_kernel_tests.sh` |
| RFS | `nice -n 19 bash scripts/test/host_fs_tests.sh` |
| RNET | `nice -n 19 python3 -B tests/host/net/settings_run.py` |

If a later directive changes abi.h or a public SDK header, add every new
record/constant to tests/host/abi_layout_test.c, one struct field per line in
abi.h/public copies; run host_kernel_tests.sh's ABI section (the full RK runner
also executes it) and `python3 scripts/gen_api_reference.py`. No ABI-only CLI
switch exists today; do not invent one. Code vendoring that includes a .c file
requires an nm check for duplicate defined external symbols across libraries.
These conditions do not occur in this documentation change. Preserve each
existing runner's sanitizer settings; do not disable leak detection elsewhere.

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
| SU-05 object policy | Bob/Guest attempt account-store/owner/hive backup reads; write protected Windows/System, Programs, Program Files, machine settings and Alice profile through native/Win32/DOS aliases. Denied consistently; own-profile writes succeed. Bob creates C:\GAME and a root file; creator/admin can edit/delete, other Users read/execute, Guest read-only. Check the 256-entry bound, owner-record crash/reuse/rename, another fixed volume and no system alias bypass. USB file writes succeed for Bob, deny for Guest. Admin raw reads/writes of the system disk deny; authorized SYSTEM maintenance still works. |
| SU-06 applets | As Bob view device/network/time/power/display/default-program pages, attempt disabled edits through UI and direct transport. Machine writes denied, personal edits persist only to Bob. Alice's settings stay unchanged. |
| SU-07 profile/Win32 | Alice then Bob: different Desktop/Documents/CSIDL, temporary directory, HKCU value and GetUserName; HKLM writes denied to Bob, accepted only as authorized administrator. Cross-user/old-session token and hive handles invalid/denied. |
| SU-08 installers/games | Administrator completes the selected VISE, InstallShield and NSIS fixtures with inherited credentials and correct menu/Uninstall scope. Bob's protected all-users install fails visibly; supported own-profile install and beside-exe save succeed. No version-based elevation. |
| SU-09 lock/saver | Passworded Alice locks from menu/secure attention, then passworded saver dismissal in normal and DOS/Win32 full-screen modes. Wrong password/Cancel/synthetic input and ordinary desktop crash preserve concealment; correct password restores the same session. Plaintext never appears in app input, clipboard, capture or serial log. Blank CiukiUser is told to set a password, not shown a false secure lock. |
| SU-10 resume | On a supported virtual standby provider, arm password-on-resume, suspend, wake; no application frame/input before trusted lock. Wrong password remains locked, correct password resumes. Unsupported provider is explicitly not run, not counted as a pass; physical confirmation remains outstanding. |
| SU-11 logoff | Open unsaved native/Win32 documents plus DOS VM. Veto leaves same session; consent closes all. Re-login Bob shows no Alice process, temp/hive handle or clipboard content. Switch User remains unavailable with reason. |
| SU-12 recovery accounts | Safe boot shows 500; test M-Q1/M-Q2 with correct/wrong/unset password, then normal enable/disable. Set-password recovery resets inactive Bob only through service; no online passwordless recovery. With all administrator verifiers lost/corrupt, use the explicit confirmed Setup CD repair path, inject interruption and verify preserved ownership/nonreuse and a new configured recovery password. Disabled Guest never acts as fallback. |
| SU-13 crash/migration | Use runtime fault selection on disposable overlays: kill account service mid-management, leave stale v1 lock, interrupt each account/owner/migration publication stage, restart. Only a coherent old/new state or explicit closed recovery appears; no duplicate uid/admin, stale password revival after acknowledged reset, lost profile data or writable credential copy. |
| SU-14 shutdown | At Welcome choose Shut Down/Restart; fixed trusted power request succeeds with no desktop. Logged-in Bob's local shutdown follows save/veto; machine-policy edits still denied. |

### 10.2 Documentation and inventory migration

Keep G32's recorded implementation history and mark its profile-only security
rules superseded. Keep the seven requested inventory rows with the new acceptance
boundary, preserving earlier host evidence as historical evidence only:

| Row | Completion requires |
| --- | --- |
| SS-1 | Authenticated Welcome/logon, real uid/groups, private profiles and Win32 username/HKCU/CSIDL, SU-01..07. Network roaming remains out of scope. |
| SS-2 | Service-backed self password/reset policy and merged User Accounts; no default-profile/logon-disable bypass. Network password provider remains out of scope. |
| SS-3 | Kernel-backed trusted lock with saver path and full-screen revocation, SU-09; an opaque desktop overlay alone does not close it. |
| SS-4 | Authorized create/delete/type/password/picture and Guest/500 enablement; active-account EBUSY, last-admin/nonreuse, cross-store crash recovery, explicit G32 enrollment and Setup CD repair. G32 CRUD alone does not close it. |
| SD-6 | Veto-aware teardown of the actual credential session, return to authenticated Welcome without auto-logon or Cancel escape, SU-11. |
| PW-8 | Existing lid/button behavior plus trusted password-on-resume ordering, SU-10, with qualified provider evidence explicitly identified. |
| DS-3 | Existing saver controls plus functioning per-user password setting and trusted dismissal hook; do not overwrite existing G10/DPMS limitations. |

Before integration, the lead adds explicit supersession pointers at affected
sections of execution, VFS, unified DOS, Win32, registry, POSIX, Setup, saver and
power contracts, updates SDK/user documentation and the relevant applet catalogue
aliases, and records the binding decisions through the lead's normal diary
process. This implementer does not edit the diary, source, SDK or test runners.

## Decisions

Binding source: [sec-lead-decisions.md](../directives/sec-lead-decisions.md).
M-Qn means its sec-model Qn answer; K-Qn means its sec-kernel K-Qn answer.
These close the former open-question tables in both drafts. Interface details
are frozen in the kernel half; account/owner transactions, service storage and
consumer behavior are coordinated in the main half.

| Decision id | Applied decision / location |
| --- | --- |
| M-Q1 | Configured uid 500 may authenticate with its password in Safe Mode while disabled normally; visibility alone is insufficient. Both account tables and recovery flows use this exception. |
| M-Q2 | Ready images leave 500 unset and unable to authenticate until an administrator sets its nonempty password in User Accounts; no distributed recovery password. |
| M-Q3 | One credential/peer ABI, SYSTEM gid/group 0, retained sender identity, active-account EBUSY, coordinated owner/account commit (main section 3.5), service-only authoritative hive files, trusted physical Ctrl+Alt+Delete and session drain. Privilege queries are the frozen read-only view. |
| M-Q4 | Explicit restartable G32 enrollment chooses first administrator; other imported users are Standard with fresh nonreused uids; ordinary boot stops until committed. |
| M-Q5 | 8-63 printable ASCII bytes for custom users/500/Guest; sole enabled blank exception ready-image CiukiUser. Missing unattended secrets fail before writes; secret transport deferred. Historical NIST rationale is identified without a current-compliance claim. |
| M-Q6 | Per-install machine SID from entropy before account publication on first boot, protected, durable and idempotent. Ready clones do not ship one published machine identity. |
| M-Q7 | Standard root files/folders use Creator Owner full control, administrators full ordinary-data control, other Users read/execute, Guest read-only. Approved Guest limits include read-only removable media. Keep personal/machine split, 10,000 KDF pending PIII measurement and no persistent lockout. |
| K-Q1 | Private numeric profiles under /Windows/Profiles, SYSTEM metadata under /System/Security; explicit old-root to uid >=1000 mapping, never 500/501. Shared /Documents stays shared; deletion retires owners permanently. |
| K-Q2 | SYS_SECURITY=101 is frozen in the kernel half with three layouts, seven operations, peer fd, profile descriptor, errors/flags/bounds and the read-only SID/token facade. W32_DISPLAY remains under call 96. |
| K-Q3 | 32 policy rules, 32 profile roots, 256 creator-owned root files/folders per volume. Additional fixed volumes follow the same root rule as data volumes, never unprotected system aliases. |
| K-Q4 | Sealed SYSTEM-only boot/service set; all user raw reads/writes on managed system disk refused pending exact sector classification. Maintenance/Setup acts through SYSTEM after admin checks. Common prefix policy replaces Win32 all-/System denial; F5 builtin-store protections remain. |
| K-Q5 | Per-session user w32srv plus SYSTEM machine-registry broker; no SYSTEM desktop. All authoritative hive backing files service-only; HKCU/HKU/.DEFAULT correctly bound; personal non-authoritative libreg preferences may remain owner-writable. |
| K-Q6 | Exact XP default parity is unnecessary; document CiukiOS choices. No persistent lockout, one-second bounded retry. Historical gaps are not implementation blockers. |
| K-Q7 | Ready 500 unset, 8-63-byte custom/Guest passwords, and explicit Setup CD account repair if all administrator credentials/verifiers are lost or corrupt. No implicit empty recovery. |
| K-Q8 | Local shutdown/restart for all interactive users including Guest and on logon screen; mixer/display/input preferences per session, machine defaults admin-only. Sec-k04 audits INPUT_KEYBOARD_SET and CIUKI_SETUP_NET_QUERY and refuses unsupported routes until fixed. |

No new policy question remains open in this reconciliation. Lead cross-review,
implementation, target KDF measurements, runtime security/installer evidence and
hardware qualification remain outstanding. The proposed security QEMU suite
still needs the lead's existing policy authorization before execution; this
document grants none. No runtime evidence is claimed by the design.
