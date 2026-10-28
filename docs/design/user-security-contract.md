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

Review round 1 applies [Lead amendment 1](../directives/sec-contract.md#lead-amendment-1-2026-10-26-1145-cross-review-round-1),
identified as A1-1..A1-11 in both Decisions tables. It closes all eleven review
findings: a sole-account, irreversibly retired blank-autologon exception; explicit
role-bound session launch/secure-console and service-connect operations; one
batch owner transaction; exact seeds and persistent bootstrap evidence; protected
bundled installers; unchanged DOS C:/D:/E: roles; identity-bound restart journals;
native libreg transport; profile console/TEMP consumers; and corrected Safe Mode
attribution. The final **Delta for sec-k01 and sec-user-01** section enumerates
interface changes relative to 3ddc9de88 for the implementations already underway.

Review round 2 applies [Lead amendment 2](../directives/sec-contract.md#lead-amendment-2-2026-10-26-1245-cross-review-round-2),
identified as A2-1..A2-8. It supersedes round 1 where necessary: authentication
has a complete role-bound challenge relay; DESKTOP can request only its own
session's lock; console/authentication epochs prevent stale release; cancellation
is best effort before server admission with durable ACCOUNTS status afterward.
OWNER resolves CREATE roots and validates a shared staged manifest. Bounded
checkpoint windows replace lifetime receipts; metadata/password updates do not
consume owner receipts. Blank CiukiUser replacement is password-first, with no
atomic replacement operation. Authorized PROFILE queries return mapped roots.
The final delta separately identifies changes relative to **f3b388722**.

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
| R4 | Microsoft Press XP Professional Training Kit, [Understanding User Accounts](https://flylib.com/books/en/2.732.1.17/1/) (mirror): Guest disabled by default; built-in Administrator omitted from Welcome under the documented workgroup/Setup conditions. It does not establish XP Home Safe Mode behavior. | M-Q1's passworded Safe Mode exception is an explicit CiukiOS decision. [KB814777](https://learn.microsoft.com/en-us/troubleshoot/windows-client/windows-security/access-computer-after-administrator-disabled) describes a Server/RIS scenario, not universal XP edition defaults. |
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

Round 1 also checked `channel.c:307-326`, `win32/service.c:191-242`,
`grants.c:52-77`, `supervisor.c:109-145`, `sdk/libreg/registry.c:320-366`,
`apps/w32srv/restart.c:30-35,64-100`, `kernel32/console.c:33-52` and
`services/start.c:58-66,227-233`. These have no security-role connector, batch
owner protocol or secure handoff yet; libreg reads raw files, restart state is
machine-global/unowned, and console/default environment use global TEMP.
The amendments below are required future changes, not descriptions of delivered
enforcement. Microsoft [desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)
supports isolating input/presentation; the precise call-101 protocol is CiukiOS's
decision. Microsoft's [DLL search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order)
and [MS02-064](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064)
explain writable search-path planting; the protected store below follows from
that risk, without claiming current DLL defaults are XP defaults.

Round 2 rechecked Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
(process security context), [desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)
(only one input desktop active; restricted Winlogon domain), and
[cancellation considerations](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
(cancellation request alone does not prove an operation was cancelled). These
support retained role identity, separate presentation domains and explicit
outcome observation. The challenge codecs, epochs, reservation/status windows
and checkpoints below are CiukiOS decisions, not claims that Windows used this
protocol. Current cancellation documentation mentions Vista extensions; it is
used only for the general completion/cancellation distinction, not XP API parity.
The Open Group [pwd.h](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/pwd.h.html)
and [getpwnam](https://pubs.opengroup.org/onlinepubs/9799919799/functions/getpwnam.html)
retrieval again returned HTTP 403; PROFILE's filtered native subset is explicitly
specified here without a newly verified POSIX-conformance claim. Repository
checks of `channel.c:307-326`, `win32/service.c:191-242` and
`posixpath.c:115-122` confirm queue/lifetime reuse is possible but owner locators
cannot come from current namespace-local inode numbers. The built payload map
in `scripts/build_image.py:347` supplies `/Programs/terminal`, the fixed shell
information value below; no nonexistent `/Programs/shell` is proposed.

## 3. Accounts and credential storage

### 3.1 Account lifecycle

| Account | Creation and groups | Enablement and logon |
| --- | --- | --- |
| SYSTEM, uid 0 | Internal kernel/boot/logon identity, never an interactive account. | Never listed, never password authenticated, never selected by name. |
| CiukiAdministrator, uid 500 | Reserved built-in recovery administrator, groups 544+545. Cannot be deleted, renamed or converted to Standard. | Disabled and hidden in normal boot by default. Setup sets a password. M-Q1 permits password-authenticated Safe Mode logon while it stays disabled normally; an unset verifier still cannot authenticate (M-Q2). Explicit administrator enablement makes it available at normal logon. |
| Guest, uid 501 | Reserved, groups 546 only; cannot be deleted or made administrator. | Disabled by default. M-Q5 requires an administrator to set an 8-63-byte password before enablement. No failed/unknown-name fallback to Guest. |
| CiukiUser, uid 1000 | Only ready-made images that bypass Setup; groups 544+545. It is an ordinary account despite the distribution default name. | Initially enabled/passwordless with boot autologon, only while it is the sole enabled normal-boot interactive account and the ready exception has never been retired (A1-1). |
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
its password is set. Disabled recovery uid 500 does not count as a
normal-boot administrator. Other administrators can make changes after the user
logs off. Password/picture changes do not replace an immutable process credential.
Deletion retains files and protected owner metadata; the UI explains this and
offers administrator-managed backup separately. Guest data is persistent but
shared between everyone using that one account; do not imply individual privacy.

A1-1 is an additional invariant checked against the complete proposed database,
including Guest and normally enabled uid 500. Before enabling any other
interactive account, CiukiUser MUST first have a committed nonempty password
(A2-7). Creating a disabled account or configuring disabled uid 500's password
is allowed. Renaming/demoting blank CiukiUser also requires a password first;
active-account EBUSY still applies to disable/demotion. Refuse a violating
request before persistent writes; merely
clearing auto_logon_uid is insufficient because manual empty logon would remain.
Replacement uses ordinary single-target operations: set CiukiUser's password,
create/enable another password-configured administrator, log off CiukiUser and
log on as that administrator, then disable/demote CiukiUser. Every intermediate
database obeys last-administrator and active-account rules. Disabling the sole
blank administrator fails last-admin protection; it is not an alternative to
password-first replacement. No intermediate database may enable both another
account and blank CiukiUser. Setting CiukiUser's password or later changing its
name/type permanently sets header flag auto_logon_retired (bit 2) and clears
auto_logon_uid/ready_blank_exception. A disabled former blank record becomes
unset (zero KDF/salt/verifier); reenabling requires a nonempty password. No API
removes a password, restores the exception or clears the retired flag. Deleting
other users, restoring a label/type, service restart and reboot never rearm it.
There is no atomic replacement/batch-management operation. Disabling, deleting
or demoting CiukiUser later never restores the retired blank exception.

### 3.2 Password and retry policy

M-Q5 policy: 8-63 printable ASCII bytes (0x20-0x7e), case-sensitive, including spaces;
no trimming, silent truncation, forced composition, automatic expiry or password
history. Old/new/confirmation fields are masked and cleared after completion,
cancellation and error. Self-service password change requires the old password;
an administrator may reset another inactive user's password through the service,
with confirmation. The account service never returns the stored verifier.

The only enabled empty-password exception is the owner's ready-image CiukiUser,
under the sole-enabled-account and permanent retirement rules of section 3.1.
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
| 32 | u32 flags: migration_complete bit 0, uid_exhausted bit 1, auto_logon_retired bit 2 (monotonic); other bits zero |
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

Exact credential combinations in a published database are: unset = password_set
clear, KDF/iterations/salt/verifier zero and enabled clear; configured =
password_set set, KDF=1, iterations=10000, fresh 16-byte salt and 32-byte PBKDF2
verifier (a zero-valued hash is not rejected merely for its value). Configured
records obey the password policy at enrollment. The ready blank record alone
has enabled|password_set|ready_blank_exception, groups=3, name=CiukiUser,
uid=1000, origin=1, auto_logon_uid=1000, auto_logon_retired clear, and no other
enabled account; first boot derives its verifier from the empty byte string.
ready_blank_exception on any other tuple, or on an unset published record, is
invalid. change_password_required is allowed only on disabled imported records,
with KDF=1 and a validated v1 verifier; it cannot authorize a desktop. Reserved
uids have built_in set; ordinary uids do not. Disabled configured accounts keep
their verifier. Header/record consistency is validated before logon, not just
when the applet writes it. Seed-only combinations below never authenticate.

### 3.4 Serialization, replacement and recovery

Use one live SYSTEM service and an in-memory serialized mutation queue. Clients
cannot write the store or acquire a persistent management lock. Associate pending
requests with authenticated channel identity and durable client operation id
(section 4.1); client death cancels work only before COMMIT_REQUIRED, after which
explicit SYSTEM recovery finishes the admitted intent and STATUS reports it. The kernel must prevent a second live account-service
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
valid generation only before COMMIT_REQUIRED. After that durable point, finish
the intended generation even if the reply was lost. An acknowledged password reset
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

### 3.5 Account/owner batch transaction (A1-4)

The ACCOUNT role is the sole account writer; the kernel-registered OWNER role
is the sole profile-owner writer. Use service OWNER_PROVISION through the
role-bound transport in the kernel half. uid 0 or a claimed name alone does not
qualify. SETUP may stage a destination, but only installed-boot ACCOUNT promotes
its seed. Initial Setup stages private directories using its existing destination
I/O, not owner COMMIT with a zero SID. Boot SETUP may use this owner protocol
only for the explicit confirmed repair of an already identified destination;
then it uses that installation's nonzero SID/key and the same recovery rules. The same batch codec/fault fixtures serve sec-k02 and sec-user-01.

One ownership-changing operation carries **every** binding: three accounts/profiles at bootstrap,
or up to sixteen imported profiles plus both built-ins (18 bindings), bounded
by the 32-profile owner capacity including retained roots. A disabled built-in
still gets a protected owner binding. No per-binding accounts.db generation.
One serialized batch reserves one transaction generation G, used as the new
accounts.db generation; aborted generations are skipped, never reused. The
protected accounts allocation intent retains the highest reserved G and next_uid
independently of the currently published database. Neither abort nor backup
restoration lowers these watermarks. G exhaustion fails EOVERFLOW before writes.

Transaction key = (three installation SID components, destination volume u64
identity, G u64). The canonical request digest covers this key, old account and
owner generations, the complete sorted binding vector, target database digest
and staged-profile manifest digest. SHA-256 detects contradictory retry content;
it is not offline authentication. Reusing a key with different content fails EIO.
Every retained-window retry uses the identical full vector. Keys at/below the
checkpoint floor return CHECKPOINTED as defined below, never execute anew.
Expected-generation checks apply
to a new PREPARE; mismatch is EAGAIN with no reservations/writes. Existing-key
PREPARE/COMMIT/ABORT/STATUS instead match the original key/digest even after
the current store generation advances; a committed retry cannot become stale. A single writer admits one batch at a time.

The service body is little-endian, no C padding. The frame in the kernel half
permits an 8192-byte reassembled body. PREPARE=1, COMMIT=2, ABORT=3, STATUS=4,
CHECKPOINT=5, COMPACT=6, RESERVE=7. Batch operations 1..4/7 use the following 160-byte header plus count 128-byte bindings (maximum
32, total at most 4256 bytes); RESERVE/PREPARE/COMMIT/ABORT/STATUS repeat the complete immutable
request, not just a uid. The operation word is excluded from the content digest.

| Header offset | Field |
| --- | --- |
| 0 | u32 version=1 |
| 4 | u32 header_bytes=160 |
| 8 | u32 operation (1..7; checkpoint forms below) |
| 12 | u32 binding_count (1..32) |
| 16 | u32 machine_sid[3] |
| 28 | u32 reserved=0 |
| 32 | u64 destination_volume_id |
| 40 | u64 transaction_generation G |
| 48 | u64 expected_accounts_generation (0 only before first publication) |
| 56 | u64 expected_owner_generation (0 only for an empty owner store) |
| 64 | byte target_accounts_sha256[32] |
| 96 | byte staged_manifest_sha256[32] |
| 128 | byte request_sha256[32] (this field zeroed for digest calculation) |

| Binding offset | Field |
| --- | --- |
| 0 | u32 uid |
| 4 | u32 action: CREATE=1, RETIRE=2, KEEP=3 |
| 8 | u64 owner_key (zero for CREATE, existing key otherwise) |
| 16 | u64 expected_binding_generation (zero for CREATE) |
| 24 | u64 destination_volume_id, matching header |
| 32 | u64 resolved_root_locator=0 on every request; OWNER-private identity, never caller input |
| 40 | u32 path_bytes (including NUL, 1..64) |
| 44 | u32 policy_kind=1 (private profile) |
| 48 | char canonical_path[64], zero-padded |
| 112 | byte reserved[16]=0 |

Sort by uid, reject duplicates, unknown actions, aliases, stale/recycled owner keys,
uid/root collisions and path overflow before reservation. KEEP proves
an existing binding; RETIRE preserves data/key/uid under retired ownership.
Neither deletes an owner's data. CREATE cannot claim an unrelated root.
Bootstrap uses CREATE for 500, 501 and 1000; sixteen-profile migration uses 18
CREATEs. Ownership-neutral picture/label/password/type/enable updates use only
ACCOUNT's atomic database/result journal, **no OWNER transaction or receipt**;
an unchanged binding is verified against the current owner checkpoint. ACCOUNT
includes bootstrap.state retirement flags in its own local durable intent when
setting the ready account's first password; no owner receipt is required for
that irreversible header/state update. Renames
of account labels never rename roots. CREATE/RETIRE or a changed mapping uses
the owner protocol; KEEP may accompany other bindings in that same batch.

**OWNER-issued destination identity (A2-5).** ACCOUNT supplies canonical paths
and zero locators for every action. OWNER resolves the destination from its mounted-volume
identity and the protected staged manifest, under its own resolver/namespace
reservation. It alone allocates a durable monotonic locator/owner key and
persists that path-to-object reservation in RESERVE/PREPARED evidence before
replying. A retry still supplies zero; OWNER recovers its saved locator rather
than treating a slot, LBA or st_ino as identity. For KEEP/RETIRE, OWNER resolves the existing
locator from owner_key/binding_generation/path in its committed store; ACCOUNT
never needs a locator query or private locator value. Locators are never reused,
even on ABORT, deletion, mount teardown or reboot. Reservation fences root and
ancestor replacement/rename/removal, aliases and slot reuse until commit/abort;
recovery re-resolves and validates the saved manifest/object mapping under that
fence before restoring it. A mismatch closes recovery, never binds a replacement.
`proc/posixpath.c:115-122` allocates namespace-local inode counters; these cannot
supply persistent owner locators. No ACCOUNT->destination-locator query is needed.

Both sides read **the same immutable manifest bytes** at the deterministic
SYSTEM-only destination path `/System/Security/Staging/g%016x/manifest` (G in
hex). ACCOUNT stages/flushes it, then OWNER pins its generation and hashes it
independently before reserving anything. Its reservation seals manifest/tree
bytes against every writer, including ACCOUNT, until terminal cleanup; COMMIT
revalidates the pinned object/map evidence before publication. ACCOUNT cannot pass a source-host fd,
path to a different volume, or a digest without accessible matching bytes.
Manifest schema 1 is little-endian: 64-byte header (magic[8]=`CIUKISTG`, version
u32@8=1, header_bytes u32@12=64, entry_count u32@16<=32, reserved u32@20=0,
SID[3]@24, reserved u32@36=0, volume_id u64@40, G u64@48, reserved[8]@56=0),
followed by entry_count 256-byte entries sorted by new uid. Each entry has uid
u32@0, original_G32_id u32@4 (0 fresh), action u32@8 (binding action), flags
u32@12 (fresh=1, imported=2, relocated=4; relocated requires imported),
canonical_root[64]@16, prior_root[64]@80 (empty unless imported), staged_root[64]
@144 (SYSTEM-only `/System/Security/Staging/g%016x/p%08x`), staged_tree_sha256[32]
@208, old_owner_key u64@240 (0 fresh), reserved[8]@248=0. Maximum 8256 bytes;
this file is read from storage, **not** sent as an 8192-byte service body.

The tree digest is over a recursive canonical-path-sorted stream excluding dot
entries: u32 relative_path_bytes including NUL, path bytes (ASCII resolved long
name, case-folded for ordering/duplicate detection), u32 kind (directory=1,
file=2), u64 file_length (0 directory), then SHA-256(file contents) for files
and 32 zero bytes for directories. Include the root as empty relative path
(one NUL) with kind directory. Reject unsupported node types, alias collisions,
paths escaping staging, cross-volume links and any concurrent generation change;
no timestamps, host inode numbers or source-volume locators participate.
ACCOUNT and OWNER independently verify header/key/digest, one-to-one binding
correspondence and root syntax; OWNER additionally verifies existing ownership,
resolved destination reservations and protected enrollment old-id mapping.
ACCOUNT additionally verifies target accounts.db digest/uid set and enrollment
choices against its protected intent. A G32 prior root must match that intent's
old-id/new-uid map; ordinary CREATE cannot claim imported flags. Relocation
copies from that proven old root into destination staging, verifies all bytes,
and keeps the prior root deny-labelled until commit/cleanup; neither side
trusts a caller-invented locator. Profile directory installation uses these
reserved roots before publication; USER.CRG/private hive files stay service-only.
Any manifest or tree change requires ABORT and a new G; it cannot mutate a retry.

Reply: 96-byte header then count 32-byte results in request order. Offsets:
version u32@0=1, header_bytes u32@4=96, state u32@8, count u32@12,
machine_sid[3]@16, reserved u32@28=0, volume u64@32, G u64@40,
committed_owner_generation u64@48, reserved u64@56=0,
request_sha256[32]@64. Result: uid u32@0, index u32@4, owner_key u64@8,
binding_generation u64@16, action u32@24, reserved u32@28=0. The service frame
carries negative errno on failure and no body; outputs are not partially applied.
RESERVE reserves every key and **one** next owner generation for the whole batch;
PREPARE confirms those same reservations,
COMMIT assigns that same generation to every changed binding, not one per item.
KEEP results retain their prior binding generation. A successful COMMIT returns
the exact committed profile descriptors' keys/generations. Owner generations
are durable monotonic reservations too; aborted values are never reused.

Owner states: UNKNOWN=0, PREPARED=1, COMMITTED=2, ABORTED=3,
CHECKPOINTED=4, RESERVED=5. RESERVE validates the complete immutable request and
manifest, reserves a receipt slot **including a possible early ABORTED receipt**,
root/locator/key capacity and owner generation, then persists RESERVED before
ACCOUNT may publish its RESERVED intent. PREPARE requires that reservation;
retries of RESERVE return it unchanged. An abandoned account G with no successful
OWNER RESERVE needs no OWNER ABORT/receipt; the account allocation watermark still
retires G. If RESERVE's reply is lost, STATUS/retry resolves it before reuse of
capacity. ABORT of RESERVED records ABORTED without exposing a root. ABORT of
an unreserved UNKNOWN key returns UNKNOWN and consumes no receipt; it cannot
bypass reservation limits. Thus a single batch consumes exactly one slot,
whether committed, aborted before PREPARE or abandoned after reservation.

ABORT is idempotent before COMMIT; after COMMIT it returns EBUSY. STATUS after
restart returns durable state and identical retained results. UNKNOWN and
CHECKPOINTED have count=0, owner_generation=0 and no results. COMMIT of UNKNOWN,
ABORTED or CHECKPOINTED fails EIO. Duplicate retained COMMIT returns its original
receipt. Each RESERVED/PREPARED/COMMITTED/ABORTED slot counts toward a bounded
1024-entry window; monotonic uid/G/locator/owner-key watermarks and retired owner
bindings are permanent, **individual transaction receipts are not**. Only one
unfinalized owner batch may exist. Compact proactively at 768 entries, retaining
the most recent 256 terminal receipts when possible. New ownership operations
compact before reserving; capacity pressure is EAGAIN pending recovery/compaction,
not a lifetime ENOSPC. Real profile/root limits still return ENOSPC. Passwords,
labels, pictures and other unchanged-binding account management bypass this log.

**Checkpoint/compaction (A2-6).** ACCOUNT and OWNER share a monotonic finalized
G floor C. No outstanding RESERVED/PREPARED/COMMIT_REQUIRED batch may lie at/below
C. ACCOUNT first finishes/aborts it; new work is serialized behind this short
checkpoint. CHECKPOINT=5 and COMPACT=6 use the 160-byte batch header with
binding_count=0, transaction_generation=C, current coherent account/owner
generations, target_accounts_sha256 of current database and staged_manifest_sha256
of the checkpoint image (no bindings). The checkpoint file at
`/System/Security/Staging/g%016x/checkpoint` contains version/header/key/floor,
the full current owner map including retired bindings, all allocation watermarks,
current database digest, and the sorted retained receipt key/digest/state/result
vectors. Reuse OWNER's versioned store codec for that owner snapshot; both sides
validate the file digest and complete sorted maps before acceptance. It is
SYSTEM-only; neither sender supplies a new destination identity.

1. OWNER CHECKPOINT durably writes a new immutable snapshot and candidate C,
   returning the 96-byte header with state=CHECKPOINTED, count=0, G=C and the
   request digest in request_sha256 (the snapshot-file digest is the request
   header's staged_manifest_sha256). It retains the old snapshot/log; candidate publication
   alone never removes a receipt. Retries must match C and digest exactly.
2. ACCOUNT verifies that snapshot against every committed profile/retired uid and
   its own completed/aborted intents, durably publishes its matching account
   checkpoint marker (C, owner snapshot digest, database generation/digest,
   allocation watermarks), and only then sends COMPACT with the identical header.
3. OWNER COMPACT verifies its candidate and ACCOUNT's protected durable marker,
   atomically installs the snapshot and floor, flushes, then removes only log
   prefixes <=C. ACCOUNT verifies STATUS/checkpoint floor before cleaning its
   old owner intents. Keep the last coherent checkpoint until both new markers
   are durable; repeated CHECKPOINT/COMPACT of the same pair is idempotent.
4. Recovery before ACCOUNT's marker retains/replays the old log and may retry or
   discard only the unused candidate. After ACCOUNT's marker, complete the same
   COMPACT before admitting new owner work. After OWNER's floor publication,
   ACCOUNT finishes cleanup. Missing/mismatched snapshots/markers fail closed;
   no backup restore may lower either floor or any allocation watermark.

For any key G<=C, STATUS/PREPARE/RESERVE/ABORT returns CHECKPOINTED with no original
result (COMMIT fails EIO); none can execute again, even with changed request bytes.
Retained-window keys still require their original digest. A caller needing a
result must query before its window expires; ACCOUNT's client STATUS separately
returns EXPIRED after its own checkpoint, never invents an outcome. Aborted
receipts compact by the same protocol and remain nonreusable through the floor.
Under healthy storage both windows reclaim capacity indefinitely; no ordinary
self-service stream can exhaust owner management permanently.

Durable state machine, with barriers after each transition:

1. ACCOUNT persists its G/uid allocation watermarks, stages and flushes the
   manifest, and obtains durable OWNER RESERVE (including a possible ABORTED
   slot). Then ACCOUNT writes RESERVED intent: key/vector/digests, old/new database generations,
   allocation watermarks and staged manifest. Roots/hives are staged on the
   **destination volume**, inaccessible under deny-first labels; cross-volume
   rename/copy is not a commit. Flush all data, directories and destination
   volume metadata before PREPARE. Source session/media are never mutated.
2. OWNER validates its full-vector reservation and manifest, writes PREPARED
   and returns the keys. ACCOUNT records PREPARED and flushes accounts.db.new,
   containing every intended enabled/disabled account and its final SID/flags.
3. ACCOUNT writes an irrevocable COMMIT_REQUIRED intent, then OWNER commits the
   entire vector and its receipt durably. ACCOUNT records OWNER_COMMITTED. No
   individual root becomes ordinary shared data; no session is admitted yet.
4. ACCOUNT installs accounts.db G using section 3.4 barriers, records DB_INSTALLED,
   then DONE. Bootstrap also reaches CONSUMED below before session admission.
   Only after durable DONE is success acknowledged. Old database backup is
   recovery evidence, not permission to undo an acknowledged password reset.
5. ABORT before COMMIT_REQUIRED retires all reserved ids/keys/G, removes only
   unexposed staging and writes durable ABORTED in both stores. After the
   irrevocable point, cancellation suppresses replies but recovery finishes
   the commit; it never compensates with uid reuse or partial owner rollback.

| Durable evidence on recovery | Required result before accepting any client |
| --- | --- |
| Owner RESERVED but ACCOUNT intent absent | Validate the saved manifest/key and durably ABORT the orphan reservation before checkpoint; G/uid/key watermarks never decrease. |
| No reservation / no owner receipt | Old coherent state; a demonstrably fresh seed alone may begin bootstrap. |
| RESERVED or PREPARED, owner UNKNOWN/PREPARED, no COMMIT_REQUIRED | Complete PREPARE or durably ABORT the **whole** batch; preserve all watermarks. |
| COMMIT_REQUIRED, owner PREPARED/UNKNOWN | Validate complete flushed staging and resume PREPARE/COMMIT; missing/corrupt staging closes recovery, never publishes part of the batch. |
| Owner COMMITTED, account old/missing/.new only | STATUS authenticates key/digest; finish intended database G from the verified staging. If unavailable, remain closed with retired/reserved owners, never recreate defaults. |
| Database G installed, owner PREPARED (unexpected ordering) | Block logon; with matching COMMIT_REQUIRED and valid staging finish owner COMMIT. No account is usable before both commit. |
| Database G installed, owner UNKNOWN/ABORTED or digest mismatch | Contradictory state: remain closed for repair; never invent bindings or downgrade to an old password. |
| Owner COMMITTED and database G installed, intent not DONE | Verify digests/generations and complete DONE/seed consumption. |
| DONE or success already acknowledged | Preserve G, owner receipt and password changes; clean only proven obsolete staging/backups. |
| ABORTED | Old database remains authoritative, all reservations retired; STATUS stays ABORTED within the retained window, then CHECKPOINTED. Any visible G is contradictory and closes recovery. |

**First-boot uid recovery (sec-sysvc amendment, lead decision 2026-10-27).**
The watermarks still never decrease, but a uid whose **only** OWNER
reservations were aborted before **any** batch committed may be reserved again:
OWNER admits a CREATE below `uid_high` only while its complete history proves
nothing was ever published (no committed owner generation or account batch, no
checkpoint floor or candidate, no profile record, no batch in flight, every
retained receipt ABORTED) and at least one aborted receipt named that uid. A
power cut between OWNER's durable RESERVE and ACCOUNT's intent write on a first
boot therefore no longer leaves a fresh installation closed: the next boot
aborts the orphan as the first table row requires, and the ready/Setup recipe
is published at the next generation with the same uid 1000. Every other
retired uid (never reserved, or any uid once a batch committed) stays retired.
Evidence: H15 (power cut at that write), H02 (`docs/validation/sec-sysvc.md`).

Deletion/disable/type change of an active account returns EBUSY before reservation.
The 18-account and 32-profile bounds are independent; retained deleted roots
consume profile capacity. SESSION_CREATE accepts only committed owner descriptors,
never PREPARED or retired bindings. Recovery runs before exposing account services.

### 3.6 Exact seeds and persistent bootstrap state (A1-6)

`/System/Security/provision.seed` uses the 96+256*N encoding in section 3.3,
except magic=CIUKISEE, version=2, generation=0, next_uid=1001, count=3,
flags=0, record_bytes=256, SID components=0, reserved=0. origin is 1 ready or 2
Setup; auto_logon_uid is 1000 only for origin 1, otherwise 0. The checksum uses
the same whole-file zeroed-digest rule. Records are exactly these, sorted by uid;
original G32 id=0, picture empty, reserved/padding zero in every seed record:

| Seed | uid / name | flags / group bitmap | KDF / iterations / salt / verifier |
| --- | --- | --- | --- |
| Ready | 500 / CiukiAdministrator | 2 (built_in) / 3 | 0 / 0 / zero[16] / zero[32] |
| Ready | 501 / Guest | 2 / 4 | 0 / 0 / zero[16] / zero[32] |
| Ready | 1000 / CiukiUser | 9 (enabled + ready_blank_exception) / 3 | 0 / 0 / zero[16] / zero[32]; **seed-only** blank recipe, not an unset account permitted to authenticate |
| Setup | 500 / CiukiAdministrator | 6 (built_in + password_set) / 3 | 1 / 10000 / fresh salt / verifier of entered 8-63-byte password |
| Setup | 501 / Guest | 2 / 4 | 0 / 0 / zero[16] / zero[32] |
| Setup | 1000 / validated custom name | 5 (enabled + password_set) / 3 | 1 / 10000 / independent fresh salt / verifier of entered password |

Reject all other field combinations, extra/missing records, ready seed in a
Setup destination, nonzero seed SID/generation, shared blank verifier and unknown
origin/flags. Only installed ACCOUNT turns the ready recipe into a fresh salted
empty verifier (published flags=13), after persistent SID creation. Setup verifier
bytes are retained. Published generation is reserved G, never 0. Migration uses
origin=3 and its protected enrollment manifest/batch, not a ready seed.

A valid seed alone is insufficient. Builder/Setup writes a matching protected
`bootstrap.state` UNPROVISIONED record and flushes it with the seed on the actual
destination; ACCOUNT never creates this initial record because files are absent.
The state codec is little-endian, exactly 160 bytes:

| Offset | Field |
| --- | --- |
| 0 | magic[8]=CIUKIBST |
| 8 | u32 version=1 |
| 12 | u32 size=160 |
| 16 | u32 state: UNPROVISIONED=1, SID_READY=2, OWNERS_READY=3, ACCOUNTS_READY=4, CONSUMED=5 |
| 20 | u32 origin (1 ready, 2 Setup, 3 enrollment) |
| 24 | u64 destination_volume_id |
| 32 | byte seed_or_enrollment_sha256[32] |
| 64 | u32 machine_sid[3] |
| 76 | u32 reserved=0 |
| 80 | u64 reserved_transaction_generation |
| 88 | u64 published_accounts_generation |
| 96 | u64 committed_owner_generation |
| 104 | u32 next_uid watermark |
| 108 | u32 flags: seed_consumed bit 0, ever_provisioned bit 1, auto_logon_retired bit 2; others zero |
| 112 | u64 state_generation, positive/nonwrapping |
| 120 | SHA-256[32] of entire record with these bytes zero |
| 152 | byte reserved[8]=0 |

Initial values: SID/transaction/accounts/owner generations=0, next_uid=1001,
flags=0, state_generation=1, explicit origin/volume/hash. Enrollment uses its
manifest's next_uid bound. State replacement uses .new/.txn/.bak and the same
fsync/intent protocol as accounts; validate every candidate, intent and checksum.
States and flags never regress. SID_READY persists the generated SID and its
protected `machine.sid` copy before any owner/account publication. The SID must be a nonzero entropy-backed
installation identity, never a build-time constant. Its transition
also seals the reserved G and complete account staging digest, including the
fresh ready empty verifier, so recovery resumes that same recipe/output rather
than accepting a new blank account from a copied seed; never regenerate
an identity after this point. OWNERS_READY sets ever_provisioned, owner generation
and transaction key; ACCOUNTS_READY sets the published database generation;
CONSUMED sets seed_consumed before any session starts. auto_logon_retired mirrors
the monotonic account flag; either copy set disables the exception permanently.
Retirement flushes both records through the account intent before acknowledgement;
recovery completes the monotonic change, never clears the surviving retired bit.

| Interruption / state evidence | Recovery |
| --- | --- |
| UNPROVISIONED, exact seed/hash/volume, no SID/owner/database/reservation evidence | Only case allowed to start fresh ready blank provisioning; reserve G and persist entropy-backed SID. |
| Before SID persistence completes | Recover a matching valid state/SID intent; if no write committed, retry entropy. A conflicting SID or state is an error, not permission to replace it. |
| SID_READY; owners not committed | Reuse this SID/key and section 3.5 staging; never derive a second installation SID or treat seed as fresh again. |
| Owner COMMITTED before state reaches OWNERS_READY | STATUS + intent advance state; finish the intended database, preserving SID. |
| OWNERS_READY; database missing/old | Finish the exact G; unavailable verified staging keeps the gate closed. |
| Database G installed before ACCOUNTS_READY | Verify receipt/digests and advance state; do not authenticate yet. |
| ACCOUNTS_READY; seed not consumed | Mark CONSUMED durably, then admit logon. Crash during seed unlink is harmless because the consumed tombstone remains. |
| CONSUMED; residual seed or missing/corrupt accounts.db | Never replay seed; use verified committed recovery data or explicit repair. |
| Missing/contradictory state, seed, SID, receipt, hash or volume | Fail closed. Absence of accounts.db is never evidence of a fresh installation. |

Setup stages the full batch and seed on its destination; first installed boot
performs promotion. PID 1 starts sealed roles in session 0, recovers the state
machine and stores, and only then admits authentication. G32 boots stop at
explicit enrollment. Session launch uses the kernel half's SESSION_LAUNCH,
LAUNCH_POLL and secure-console operations; there is no unspecified supervisor
handoff or ordinary desktop launched with uid 0.

## 4. Logon, sessions, lock and power

### 4.1 Service interfaces and invariants

PID 1 starts the sealed LOGON coordinator, LOGON_UI, ACCOUNT, OWNER and REGISTRY
roles before any ordinary desktop. The kernel half freezes service ids 1..6,
CONNECT/ACCEPT/CANCEL, framing, noninheritability and retained subjects; use those
interfaces rather than a private caller-chosen pid/path or uid 0 alone.
Only the trusted logon UI may submit authentication/enrollment/unlock requests;
ordinary account-management clients get authorized operations, filtered reads and
self-password change. Kernel-provided sender identity, never a request's uid,
environment or window title, authorizes each call. Every mutating request has a
bounded copied payload and expected database generation; stale views return
EAGAIN for refresh. Return EACCES/EPERM for denied operations, EBUSY for active
accounts/transactions, EINVAL for malformed fields, ENOSPC for bounds, EIO for
storage corruption, and ENOMEM for allocation failure. No new panic path.

On successful authentication ACCOUNT reports the correlated result to LOGON,
which calls SESSION_CREATE, SESSION_LAUNCH (w32srv, then desktop) and LAUNCH_POLL
through the kernel half. Both ready handshakes must succeed before user input/
presentation is enabled. LOGON alone owns these call-101 operations; ACCOUNT
or LOGON_UI cannot invoke them merely because they run as uid 0. SYSTEM fds,
secrets and credential-creation endpoints are never inherited. All ordinary
children, including workers, native programs, Win32 clients and DOS VM owners,
inherit the user's credential. A SYSTEM broker must carry authenticated caller
context to perform permitted work, not execute client-supplied paths under its
own unrestricted authority.

Endpoint operations are allocated independently of kernel role numbers. All
bodies below are private schema 1, little-endian, zero-padded, with no attachments.
Unknown fields/selectors fail EINVAL. Frame failures have no body; correlated
application failures instead use the explicit status field in an outcome/status
body with frame status=0. No failure can be interpreted as verified identity.

| Service | Operation values / body contract |
| --- | --- |
| AUTH (1) | LIST=1 (empty, LOGON_UI or LOGON); LOGON=2, UNLOCK=3, ENROLL=4 (272-byte verification relay), CHALLENGE=5 (128 bytes), AUTOLOGON=6 (64-byte challenge only), ENROLL_PLAN=7 (32+40*N bytes). Operations 2..7 accept **LOGON only**. UI secrets reach ACCOUNT only through the trusted LOGON relay; an ordinary client cannot authenticate/create a session. |
| ACCOUNTS (2) | LOOKUP=1, SELF_PASSWORD=2, CREATE=3, UPDATE=4, DELETE=5, LIST=6, STATUS=7, PROFILE=8, RESERVE=9. LIST is empty; LOOKUP and mutations use the 312-byte management body. PROFILE uses that body with LOOKUP's zero-field rules. STATUS/RESERVE use the separate codecs below. No management operation creates a session. |
| OWNER (3) | PREPARE=1, COMMIT=2, ABORT=3, STATUS=4, CHECKPOINT=5, COMPACT=6, RESERVE=7; exact batch/checkpoint codecs in section 3.5. |
| LOGON (5) | AUTH_OUTCOME=1 (304 bytes, ACCOUNT only); LOCK_EVENT=2, POWER_EVENT=3 (32 bytes, LOGON_UI only); BEGIN_AUTH=4 (64 bytes, LOGON_UI only); SESSION_LOCK=5 (32 bytes, same-session DESKTOP only); SUBMIT_AUTH=6 (208 bytes), ENROLL_PLAN=7 (32+40*N bytes), both LOGON_UI only. No role can substitute another operation by changing a frame selector. |

(sec-user-03 implementation note, 2026-10-28, for lead review; details in
[sec-user03](../validation/sec-user03.md): LOGON adds two private operations,
UI_STATE=8 (LOGON_UI only: the trusted screen to show, the ATTENTION relay and
the security-menu choices bound to the current session/console) and
SESSION_EVENT=9 (same-session DESKTOP only: the pending menu choice and the
desktop's end-of-session notice). Neither authenticates, unlocks or creates a
session; the kernel frame limit of service 5 is 9. SESSION_LOCK's progress
status is -EPERM for a password-less owner: set-password guidance, no lock.)

The 208-byte UI SUBMIT_AUTH body retains these offsets: version u32@0=1,
flags u32@4=0, attempt_id u64@8, session_id u32@16 (0 for fresh logon/enrollment),
console_generation u32@20, name_bytes u32@24, secret_bytes u32@28,
new_secret_bytes u32@32, target_uid u32@36, expected_accounts_generation u64@40,
name[32]@48, secret[64]@80, new_secret[64]@144. Lengths exclude the required NUL;
all trailing bytes are zero. Name <=31, secret/new_secret <=63. target_uid must exactly match the challenge's resolved uid. For a UID selector,
name is empty; for a NAME selector, it case-insensitively matches the original
selector, and ACCOUNT also checks it against the bound account. Name spelling
never selects a different uid after challenge issuance. LOGON/UNLOCK use an empty new_secret. ENROLL uses the policy below.
LOGON checks its live attempt before copying/relaying secrets and wipes all
transient copies on completion, cancellation or death; never log them.

Management has the same first 208 bytes plus picture[96]@208 and nonzero u64
client_operation_id@304 for mutations; attempt/session/console are zero. LOOKUP
and PROFILE set client_operation_id=0 and select exactly one uid or name (zero
other selector); DELETE uses target_uid. SELF_PASSWORD is own uid, old/new secret,
zero flags; CREATE/DELETE/type/enable/password reset need current administrator
rights. UPDATE selects enabled bit 8, type bit 9, name bit 10, password reset bit
11, picture bit 12; value bits enabled bit 0 and administrator bit 1 apply only with their
selectors. Unknown/inapplicable bits or nonzero unselected fields fail EINVAL.
CREATE uses value bits only. LOOKUP/PROFILE/DELETE zero flags/secrets/picture.
Picture/self-label updates follow section 3.1. target_uid never identifies the
caller. Expected database generation is checked at initial admission; exact
operation-id retries retrieve the prior result before any new generation check.

LIST/LOOKUP replies retain version u32@0=1, count u32@4 (<=18), generation u64@8,
then count 144-byte public records (uid@0, group_bitmap@4, public enabled/type
flags@8, reserved@12=0, name[32]@16, picture[96]@48). AUTH LIST hides ineligible
accounts. Public records never include salts, verifiers or private paths.
Successful management mutations return one u64 new database generation.

#### Complete authentication exchange (A2-1)

BEGIN_AUTH is 64 bytes: version u32@0=1, action u32@4 (LOGON=2, UNLOCK=3,
ENROLL=4), session_id u32@8, console_generation u32@12, selector_kind u32@16
(UID=1 or NAME=2), intended_uid u32@20, name[32]@24, reserved[8]@56=0.
UID selects a nonzero uid with name all zero; NAME selects a valid account name
with intended_uid=0. Normal logon/enrollment session/console inputs are zero;
unlock supplies the current locked session/console and may select only its
owner. ENROLL selects a record in ACCOUNT's protected, provisional enrollment
catalogue (including the chosen first administrator and recovery uid 500).
The trusted enrollment UI stages that catalogue using LOGON ENROLL_PLAN=7,
relayed unchanged by LOGON to AUTH ENROLL_PLAN=7 on its own role-bound connection.
The plan is 32-byte header: version u32@0=1, count u32@4 (1..16),
first_administrator_old_id u32@8 (0 only for a fresh custom record), reserved
u32@12=0, expected_enrollment_generation u64@16 (0 only initially), reserved[8]
@24=0; then count 40-byte rows: original_G32_id u32@0, name[32]@4,
reserved u32@36=0. Every protected G32 record appears exactly once, sorted by old
id; without G32 data there is exactly one old_id=0 custom row. ACCOUNT compares
the list with its own protected source snapshot, validates labels/duplicates,
assigns uid 1000 to the selected first administrator and fresh uids in old-id
order to the others, and stages disabled 500/501 itself. The plan cannot supply
roles, paths, existing owner identities or verifier bytes. It durably publishes
the provisional catalogue/enrollment generation, returning the LIST public codec
with that generation and staged uids; BEGIN_AUTH ENROLL resolves only against it.
An exact plan retry returns the same map; a changed confirmed plan retires its
old reserved ids/attempts and reserves a fresh generation before staging. No
ordinary session may exist while staging an initial plan. Before a plan exists,
LIST reports enrollment-required and no ENROLL challenge is issued.

A challenge is exactly 64 bytes:

| Offset | Field |
| --- | --- |
| 0 | u32 version=1 |
| 4 | u32 action (2 LOGON, 3 UNLOCK, 4 ENROLL, 5 AUTOLOGON) |
| 8 | u64 attempt_id, fresh nonzero per boot |
| 16 | u32 intended_uid, resolved by ACCOUNT |
| 20 | u32 session_id |
| 24 | u32 console_generation |
| 28 | u32 authentication_epoch |
| 32 | u32 ui_endpoint_generation of **UI->LOGON**, not UI->ACCOUNT |
| 36 | u32 logon_service_generation |
| 40 | u32 account_service_generation |
| 44 | u32 flags=0 (AUTOLOGON uses bit 0 and no other action may set it) |
| 48 | u64 accounts_generation (protected enrollment generation before publication) |
| 56 | byte reserved[8]=0 |

1. LOGON obtains the actual UI->LOGON endpoint generation from ACCEPT/QUERY_PEER
   and a retained LOGON_UI subject. It allocates the attempt and records action,
   selector, console/authentication epoch and both registered service generations.
   One live attempt per UI connection, one active KDF plus one queued attempt
   globally. A new BEGIN invalidates the previous attempt before reuse of storage.
2. LOGON originates AUTH CHALLENGE: BEGIN_AUTH[64] followed by a proposed
   challenge[64], whose intended_uid/accounts_generation are zero and other
   bindings are kernel-derived as above. ACCOUNT trusts only its retained
   **LOGON role/service generation** to assert the cross-connection UI identity;
   it never mistakes its own LOGON->ACCOUNT endpoint generation for UI->LOGON.
   ACCOUNT resolves the selector under the current database/enrollment snapshot,
   validates state/action, retains the tuple and returns the completed 64-byte
   challenge. Unknown/ineligible names return generic EACCES without an attempt.
3. LOGON verifies the returned bindings, retains the resolved uid/generation and
   replies with that challenge to the original UI connection. The UI submits
   the 208-byte body to LOGON SUBMIT_AUTH. LOGON checks the retained selector, exact uid/action and
   all live bindings, then originates AUTH LOGON/UNLOCK/ENROLL with body
   challenge[64] + SUBMIT_AUTH[208] = 272 bytes. ACCOUNT accepts only the exact
   challenge it retained from the same LOGON instance and consumes it once.
4. ACCOUNT reports **success and failure** using AUTH_OUTCOME on its retained
   ACCOUNT->LOGON connection. The 304-byte body is challenge[64]@0, i32
   result_status@64 (0 or negative errno), u32 disposition@68 (AUTHENTICATED=1,
   ENROLL_STAGED=2, ENROLL_COMMITTED=3, FAILED=4), u32 retry_after_ms@72 (0..1000),
   u32 reserved@76=0, u64 resulting_accounts_generation@80, byte reserved[8]@88=0,
   credential[64]@96, profile[112]@160, validated account_name[32]@272.
   A failed result still carries the full original challenge; identity/profile/
   name fields are zero. The frame status is 0, including failed authentication.
   Malformed transport requests fail at the frame level and cannot authorize.
5. LOGON accepts outcomes only from the live ACCOUNT role, against the outstanding
   attempt and original UI subject, endpoint/service/console/authentication epochs,
   intended uid, action and snapshot generation. A password change invalidates
   older authentication snapshots. Failure consumes only its own attempt, never
   another connection's attempt. Cancel, disconnect, secure transition, timeout
   (5000 ms excluding an admitted KDF) or either service's restart invalidates it.
   LOGON acknowledges the outcome; ACCOUNT's verification reply carries public
   progress only. LOGON's UI reply is a 16-byte body: version u32@0=1, disposition
   u32@4, result_status i32@8, retry_after_ms u32@12; no credential is exported.
   Event loops must receive outcomes while a verification reply is outstanding.

Only AUTHENTICATED LOGON/AUTOLOGON creates and launches a session. AUTHENTICATED
UNLOCK supplies LOGON's one-use proof for SECURE_LEAVE of that same owner and
current authentication epoch. It never creates a fresh session. A failed or
stale outcome never clears authentication_required.

ENROLL is allowed only in the confirmed, protected enrollment workflow before
ordinary logon (or for an imported disabled record requiring password replacement
through that trusted workflow). The catalogue (or the retained committed migration map for a later disabled
import) fixes uid/name, original G32 mapping and administrator choice; client fields cannot change roles. For an
imported nonempty verifier, secret proves the old password and new_secret must
meet 8-63 bytes. For a fresh/empty disabled record or recovery uid 500, secret is
empty and the confirmed enrollment gate authorizes choosing new_secret. Each
success durably stages that record under the single enrollment intent and returns
ENROLL_STAGED with zero credential/profile, not a session. Once every required
choice, including the first administrator and recovery password, is staged,
ACCOUNT completes the section
3.5/6.3 batch and returns ENROLL_COMMITTED with the new database generation;
LOGON remains trusted and requires a **new LOGON challenge**. Failures carry the
same ENROLL challenge and FAILED disposition; no partial account is admitted.
Cancelled staging is resumed/aborted by its protected intent, never by an old
successful outcome. Short imported-password enrollment cannot bypass this flow.

AUTOLOGON has no UI request. On the first eligible normal boot LOGON originates
CHALLENGE with action=5, flag bit 0, UID selector=1000, ui_endpoint_generation=0,
session=0 and the current trusted console/authentication epochs (the internal BEGIN_AUTH action=5
is rejected on the UI route). ACCOUNT checks
the entire section 4.2 tuple, CONSUMED bootstrap state, initial LOGON/ACCOUNT
service generations (both 1) and a once-per-boot in-memory eligibility latch.
It consumes that latch when issuing the challenge. Registered generations are
per singleton role, start at 1 each boot and advance on every death/restart;
therefore loss of the latch through LOGON or ACCOUNT restart cannot rearm it.
LOGON also consumes its local latch before sending; explicit Log Off/Lock clears
it, and Safe Mode starts ineligible. No new latch-query syscall is required.
AUTH AUTOLOGON=6 takes only the issued 64-byte challenge, from the same retained
LOGON role. No password field or UI assertion can select this route. ACCOUNT
returns the same correlated AUTH_OUTCOME (action=5); failure stays at Welcome.
A later normal reboot may start with eligibility only while the persistent tuple
remains unretired. No automatic retry after service failure within that boot.

32-byte trusted UI events retain version@0=1, action@4 (lock=1, menu-return=2,
shutdown=3, restart=4), session_id@8, console_generation@12, attempt_id u64@16=0,
reserved[8]@24=0. Menu return uses LOGON's current epoch/cookie and cannot lower
a lock. SESSION_LOCK uses this shape with action=1, the DESKTOP's own nonzero
session, console_generation=0 and zero attempt/reserved. LOGON reads the actual
current console binding itself; DESKTOP need not obtain a privileged console query. Kernel-retained
DESKTOP role/session/process generation authorizes it; ordinary applications
ask that desktop through its existing UI route. Another session, ROLE_NONE,
W32SRV, claimed uid/name or transferred fd fails EACCES/EPERM. LOGON calls
SECURE_ENTER(LOCK), returning the 16-byte public progress shape (no cookie).
This route can only lock; it cannot authenticate, unlock, enter another session
or send menu-return/power events. A blank owner receives set-password guidance
without claiming a password-protected lock. Duplicate lock requests coalesce.

#### Durable management result and retry identity (A2-4/A2-6)

A client operation id is a globally monotonic, nonzero u64 **issued by ACCOUNTS
RESERVE and then supplied by the client**, independent of transport request_id,
fd, endpoint generation, pid and session. It is never reused across reboot.
RESERVE is 48 bytes: version u32@0=1, intended mutation operation u32@4 (2..5),
request_sha256[32]@8, reserved[8]@40=0. Hash the exact 312-byte management body
with client_operation_id zero, including its expected generation and operation
selector prefixed as a u32; secret-containing buffers are wiped, the protected
hash is not public diagnostics. Authorize the proposed operation class before
reservation; mutation admission rechecks complete fields/current rights.
RESERVE durably associates id, original requester uid, operation and digest,
then returns id u64@0 and current database generation u64@8. Clients persist the
id, operation/digest and non-secret selectors in their own protected recovery
state **before** sending a mutation. Exact retry bytes containing secrets stay
only in transient process memory and are wiped; never persist plaintext secrets.
After a client restart use STATUS, not a reconstructed secret request under the
old id. Ask for secrets for a new operation only after a definitive aborted or
expired result and a state refresh; an expired result does not imply failure.
If the RESERVE reply is lost, no mutation was sent: another reservation is safe.
At most one unsubmitted reservation per requester and one admitted writer exist;
repeated reservation retires an unsubmitted predecessor, never an admitted one.

Mutation dispatch verifies reservation owner/digest, then durably records PENDING
before any write, including metadata-only/password operations. Exact retries
return the existing outcome without repeating a password change/KDF/CREATE or
checking the now-stale expected generation. Changed content/id ownership fails
EINVAL/EACCES. A received-but-not-durably-admitted request may leave RESERVED;
STATUS may report it pending until recovery settles it, never falsely COMMITTED.
Before COMMIT_REQUIRED revocation can durably ABORT; afterward SYSTEM recovery
finishes the authorized intent without extending the dead client's authority.

STATUS is 32 bytes: version u32@0=1, reserved u32@4=0,
client_operation_id u64@8, reserved[16]@16=0. Only the original requester uid
on a live admitted connection, or ACCOUNT's explicit recovery subject, may query
a retained receipt; administrator membership alone cannot read another user's
receipt. It survives same-uid reconnection/new sessions. Response is 80 bytes:
version u32@0=1, size u32@4=80, state u32@8 (RESERVED=1, PENDING=2, COMMITTED=3,
ABORTED=4, EXPIRED=5), operation u32@12, result_status i32@16, requester_uid
u32@20, target_uid u32@24, reserved u32@28=0, client_operation_id u64@32,
resulting_database_generation u64@40, request_sha256[32]@48. Terminal COMMITTED
has status=0 and generation; ABORTED has its definitive negative errno and no
new generation; RESERVED/PENDING use status=0, generation=0 and authorize no
retry with a new id. An id above the durable allocation high-water returns
ESRCH. An allocated id no longer retained returns EXPIRED with only version,
size, state and id nonzero, to any admitted caller; it reveals no operation,
owner or result. Mutation with an expired/retired id returns EAGAIN and never
executes. Clients refresh account state before a new operation; EXPIRED never
means the old operation failed. Deletion never transfers a receipt to a new uid.

ACCOUNT checkpoints its complete coherent database/allocation watermarks plus
terminal result receipts and an operation-id floor. Retain at most 1024 entries,
compact terminal prefixes proactively at 768; keep the most recent 256 terminal
results when possible. RESERVED entries never sent for mutation may be retired
as EXPIRED; PENDING entries must finish/abort before advancing past them. Persist
the new checkpoint/floor before deleting old journal segments, keep the previous
checkpoint until that publication is flushed, and validate it on restart. The
floor and allocation high-water reject every old id, including discarded ABORTED
reservations. Receipt capacity cannot impose a lifetime mutation limit: compact
and retry admission; temporary unresolved writer pressure returns EAGAIN, actual
I/O failure EIO. Neither self-service metadata nor password changes need OWNER
space. Ordinary account administration must not require a new owner receipt for
an unchanged binding; section 3.5 compacts ownership-changing work too.

#### Authorized profile information (A2-8)

PROFILE=8 uses LOOKUP's uid-or-name selector but is authorized separately:
ordinary callers (including Guest) get only their own committed mapping;
administrators may query other committed mappings, and retained LOGON/ACCOUNT
roles may query during authenticated session preparation. Deny another user's
query EACCES before exposing paths; unknown authorized account ESRCH, inactive
unpublished/retired owner binding EACCES, changed generation EAGAIN, corrupt store
EIO. Reply is exactly 272 bytes: version u32@0=1, size u32@4=272, uid u32@8,
reserved u32@12=0, accounts_generation u64@16, owner_generation u64@24,
profile_root[64]@32, home[96]@96, shell[80]@192. All strings are canonical
NUL-terminated, zero-padded; overlength fails EOVERFLOW without truncation.
profile_root comes from the committed owner binding; home is that root plus
`/Documents`; shell is fixed `/Programs/terminal`, the existing native terminal
executable used as this subset's shell information value. This does not claim
POSIX command-language conformance or accept a client command. The kernel's
session descriptor,
not reconstructed `u<uid>` spelling, is authoritative. This adds no public
SYS_SECURITY operation or struct. sec-user-01 supplies the codec/service adapter;
sec-user-05 uses it for getpwnam/getpwuid HOME/shell and profile consumers.
Unprivileged getpwnam/getpwuid for another uid returns EACCES in this subset;
public LOOKUP remains available for non-path identity/display data. Test
uid 1007 mapped to `/Windows/Profiles/u00000003`: root/home must retain that
spelling even though `/Windows/Profiles/u000003ef` might exist for another owner.

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

Only a ready image with origin=1, enabled administrator uid 1000 named
CiukiUser, ready_blank_exception, auto_logon_uid=1000, auto_logon_retired clear
and **no other enabled interactive account** can boot directly into that account.
ACCOUNT validates the full tuple, consumed bootstrap state and empty verifier
through the normal session path. Before enabling Guest/500/another user, or
renaming/demoting blank CiukiUser, apply section 3.1's password-first rule.
Password/configuration replacement retires the exception persistently. Deleting
all other users, reverting a name/type, restarting a service and reboot never
restore it. Safe Mode, explicit Log Off and Lock never autologon even while the
fresh tuple remains valid; the next normal boot may autologon only if it was
never retired. Setup/other accounts have no autologon setting in this step.

### 4.3 Lock, saver and resume

Implement one trusted lock operation for Start > Lock, the trusted keyboard lock
entry point, password-protected saver dismissal and password-on-resume. The
kernel half's SECURE_ENTER operation must revoke ordinary display/input/full-screen access before the
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
service through AUTH; LOGON alone calls SECURE_LEAVE with the matching one-use
cookie and current session/console/grant generations and both epochs. The kernel
transition table retains authentication_required across ATTENTION and Cancel;
readiness and POLL cannot bypass a superseding gate. It never unlocks into the
other user's desktop. Lock keeps application execution alive; stale replies,
service crashes and reused descriptors cannot restore presentation.
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
| `/Documents` and `/Documents/Desktop` after migration | Explicitly shared legacy data; protected bundled-launch anchors below are more-specific exceptions. | Users read/write shared documents; Guest read-only. Not private home or a trusted installer/code search path. |
| `/Programs/Installers` and protected bundled desktop reference anchors | SYSTEM/administrator write; everyone read/execute; protected ancestors and exact shortcut targets. | No replacement, rename, deletion, adjacent DLL/configuration creation or retargeting of bundled launch references. |
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

#### Native libreg adapter (A1-9, sec-k06)

`reg_open_root`/`reg_open_root_at` currently open REG_DIRECTORY files directly
(`sdk/libreg/registry.c:320-366`). Sec-k06 owns their transport conversion and
all native machine-root consumers, not just the new broker. Machine System and
Programs reads/enumeration, generation polling, writes, transactions and
import/export use direct retained REGISTRY links. Hardware remains the kernel's
read-only inventory adapter. Filtered Users lookup uses ACCOUNTS, never raw
security files. Desktop and personal non-authoritative preference roots retain
the owner-writable profile backend; an arbitrary directory argument cannot select
an authoritative hive or create an alternate machine authority. No fallback to
REG_DIRECTORY, empty root or cached success when the broker is denied/offline.

Preserve the existing API's return/errno contract: access denial EACCES/EPERM,
stale optimistic generation EAGAIN, absent key ENOENT, read-only inventory EROFS,
bad framing/parameters EINVAL, unsupported version EOPNOTSUPP, revoked peer ESRCH,
storage/corruption EIO; reg_generation returns REG_GENERATION_ERROR on error and
never disguises denial as generation zero. Reconnect invalidates cached snapshots
and open logical handles; revalidate expected generation before mutation. Export
reads only authorized keys and writes its destination as the retained user;
import reads its source as that user and validates every destination key, atomically
rejecting unauthorized machine changes. No broker opens caller-chosen files as
SYSTEM. Compare existing applets' reads, polling, conflict/retry and import/export
against the production adapter with all direct machine backing access denied.
Bounded value/key pages carry a generation and continuation cursor; if the
generation changes, restart the snapshot or return EAGAIN. Import/export of a
root up to the existing 256 KiB limit streams bounded <=8192-byte bodies through
a broker-owned staged transaction; do not raise transport bounds or expose a raw
fd to fit a complete root. Publish import only after whole-root validation and
all key rights pass, and discard a cancelled uncommitted stream.

#### Restart journals and deferred work (A1-8, sec-k06)

Microsoft [Run/RunOnce](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)
distinguishes user and machine queues. Its current post-Vista execution details
are not adopted as XP defaults. `apps/w32srv/restart.c:30-35,64-100` instead has
an unowned v1 CIUKIRST journal and .DEFAULT RunOnce input; replace that mechanism.

Personal journal: `<profile>/Settings/Restart/personal.v2`; machine journal:
`/System/Security/Restart/machine.v2`. Both and their intents/backups are broker-only
anchored subtrees protected against owner/ancestor rename. Personal requests come
from the retained owner/w32srv session and can name only that uid's HKCU RunOnce,
profile-local installer work and owned ordinary data. .DEFAULT is never personal
input. HKLM RunOnce/RunServicesOnce and machine WININIT work enter the machine
journal only through a direct REGISTRY broker request, with an administrator check
on the original retained requester before durable admission. Per-session w32srv
cannot write/replay machine work using its own uid or a claimed forwarded identity.

Schema 2 is little-endian with a checksum, explicit installation SID/volume,
scope (personal=1/machine=2), immutable initiating uid, originating session id
for diagnostics only, account/database generation at authorization, monotonic
journal generation and nonreused job ids. Bound to 64 jobs, each <=1024 bytes;
reject overflow before persistence. Each job records kind (file mutation or
program launch), canonical source/target/executable/arguments/cwd, resolved object/
mount identities and content hashes for executable launch, permitted operation
mask, phase, and PREPARED/APPLY_REQUIRED/APPLIED/HANDED/COMPLETE/CANCELLED state.
All paths/strings are bounded by their existing API limits and the job bound;
reject instead of truncating. Expected registry generation and exact key/value
identity prevent consuming a replacement queued entry. Session ids are not
persisted credentials and never authorize replay after boot.

Replay rules:

- Personal work waits for a fresh authenticated session of the **same** uid/SID;
  reauthorize every action with its current groups and profile descriptor.
  Logoff suspends pending work after safe drain; Bob's logon never acquires or
  executes Alice's work. Password reset does not transfer ownership. Demotion
  requires fresh authorization and leaves denied work pending/cancelled with a
  diagnostic; account deletion cancels jobs and retains evidence under retired uid.
- Machine file maintenance runs only in the broker, using a recorded operation
  mask and revalidated original authorization. Permit checked ordinary installed
  program/Windows application files, not sealed boot/service inputs, security
  stores, arbitrary devices or another user's private profile. If account status
  or administrator membership changed, require a fresh administrator approval of
  the exact job/targets. Never execute a caller command as SYSTEM. Machine program
  jobs wait for the initiating administrator's fresh authenticated session and
  launch with that current interactive credential. Another administrator may
  explicitly reapprove/requeue under their own identity; Bob's login is not consent.
- Service/process crash recovers intent/generation before replay. Resolve a
  file operation's APPLY_REQUIRED against the same stable objects and receipt;
  finish/idempotently recognize it rather than act on replacement pathnames.
  Persist HANDED before executing a program. If crash leaves launch completion
  uncertain, hold for explicit reconciliation; no automatic duplicate launch or
  claim of exactly-once process execution. Revocation cancels late replies.
- Migration quarantines unowned `C:\Windows\CIUKIRST.LOG/.TMP`, WININIT and
  .DEFAULT inputs as protected evidence. Never assign them to the next logon or
  replay as SYSTEM. An administrator reviews and explicitly resubmits identified
  machine jobs; personal work without a provable owner is not automatically
  imported. Preserve original bytes for repair, mark the old schema consumed,
  and test interruption before/after that marker. Malformed v2 journals close
  that queue with an error; they do not restart from WININIT implicitly.

H07 fixtures include Alice interrupted at each phase, then Bob logs on: zero
Alice launches/writes under Bob's credential; Alice's later authorized session
resumes correctly. Include demotion/deletion, v1 unowned state, broker crash,
replacement targets, denied machine RunOnce and uncertain HANDED launch.

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

#### Profile TEMP, console backing and generated settings (A1-10)

Sec-user-05 migrates **consumers**, including AllocConsole's hard-coded
`C:\WINDOWS\TEMP` (`sdk/win32/kernel32/console.c:33-52`), automatic console
allocation (`sdk/win32/services/start.c:227-233`), that startup file's default
TMP/TEMP environment, internal application/service logs, GetTempFileName,
installer extraction/work directories and native/DOS launcher temporaries.
Interactive console/log backing goes in the authenticated profile's Temp or
Local Settings/Application Data, created exclusively with collision-safe names
and ordinary sharing/cleanup rules. SYSTEM-only internal logs stay in protected
service storage and never become a desktop stdio fd. Failure to obtain the
private directory is an error, never a writable global Windows/TEMP or /tmp
fallback. Clear stale per-session temp/console handles at logoff; private
persistent crash reports remain bound to their owner.

*sec-sysvc2 amendment (2026-10-28):* the one location of a session's own
logs and crash reports is `<profile>\Local Settings\Application Data\CiukiOS\Logs`
(the kernel LOG service's USER scope: NetSurf's `NETSURF.LOG`, `CRASHn.TXT`
native and `WCRASHn.TXT` Win32 reports); every writer and reader (NetSurf,
the desktop's crash writer, Log Viewer) uses it, and the profile-less
bootstrap session keeps its logs with the system logs. Windows XP layout and
research: `docs/validation/sec-sysvc2.md`.

The kernel half's spawn allowlist includes generated LC_ALL=C, validated TZ
(from the committed timezone parser, UTC0 if absent), canonical profile/settings
paths and fixed PATH. The current supervisor-generated values are inputs to
migrate, not justification to inherit its SYSTEM environment. The launch adapter
loads settings as the target user from the committed profile, rejects caller
path/value overrides and generates descriptor labels only for installed grants.
Win32 converts to validated per-profile DOS-spelled USERPROFILE/TEMP/TMP paths;
ordinary same-user children can use application variables, which confer no
identity or access rights. H11 runs a Standard-user console executable (automatic
and explicit AllocConsole, stdout/stderr, close/error cleanup) with global TEMP
writes denied, plus original VISE/InstallShield/NSIS extraction/child launch and
two-user console/log isolation. H06 covers private DOS short TEMP/work paths.

### 8.3 Installers and old games

VISE, InstallShield and NSIS can install for all users when already run by an
administrator, including CiukiUser on ready images. Standard users can extract
and install entirely within their profile if the installer supports it. An
all-users request fails with ERROR_ACCESS_DENIED and the shell explains that the
user must log off and log on as an administrator. This is usable without adding
a Run as credential broker to this step. Installer names/manifests and Windows
98 version probes never cause elevation. Test the original engines on the
lead's private fixtures without publishing payloads.

#### Bundled executable inventory and protected installer store (A1-5)

Sec-user-02 owns the build/Setup/catalogue migration. Bound source inspection of
`scripts/build_image.py` payload adapters, `scripts/q3a_demo.py`,
`apps/tcc/build_tcc.py:70-80,159-168`, desktop/w32srv/NetSurf builders and
`config/cd-components.json` found these `/Documents` payload classes:

| Current payload | Classification / required destination |
| --- | --- |
| `/Documents/Desktop/Quake III Arena Demo/Q3ADemo.exe` (`q3a_demo.py:17-20,46-56`) | The only staged executable found under /Documents. Move unmodified to `/Programs/Installers/Quake III Arena Demo/Q3ADemo.exe`, preserving its upstream hash/licence. |
| Same folder's Q3A_EULA.txt and README.txt | Installer documentation; move with the payload, expose read-only references from the protected presentation folder. |
| `/Documents/Developer/Guide.txt`, API/*.txt, Samples/{HelloWindow,HelloConsole}/{main.c,project.cfg} | Source/documentation only, no staged executable; remain shared untrusted editable data. Administrator/SYSTEM must not implicitly execute or build/run them as privileged startup. |
| `/Documents/Music`, Pictures, NetSurf; empty shared Desktop | Data/directories; no executable payload generated by the inspected adapters. |
| `/Programs/*`, `/System/Win32/*`, `/Games/CIUKIGAM.IMG`, `/system/dos/UNIFIED.IMG` | Executable/tool/DOS stores outside /Documents; retain protected installed-content policy and per-user COW, not a second Documents migration. |

This is a source inventory for 3ddc9de88, not a scan/build of an image or private
payloads. Sec-user-02 adds a static payload-map check rejecting any shipped PE,
ELF, COM/MZ, DLL or executable script under shared /Documents, including dynamic
adapter output and scripts without executable suffixes. New executable discoveries
must be inventoried and moved to the same protected store before that check passes;
ordinary user-authored shared documents are not trusted distribution payloads.
Update `scripts/q3a_demo.py` paths/metadata, `scripts/build_image.py` integration,
`config/cd-components.json`, Setup catalogue/static checks and launcher references.

Preserve the visible **Quake III Arena Demo** desktop folder with a protected
presentation anchor at `/Documents/Desktop/Quake III Arena Demo`, containing a
protected `Install Quake III Arena Demo.lnk` referencing the exact store executable
and protected store working directory. Users can traverse/read/launch it but
cannot replace/delete/rename/retarget its files or rename any ancestor to swap
in a lookalike; more-specific kernel anchors override the shared-parent rule.
The store and its ancestors are administrator/SYSTEM-write, everyone read/execute;
ordinary users cannot plant adjacent DLLs, INIs, scripts or executable children.
The launcher uses an absolute target/cwd, fixed trusted search directories and
clears untrusted search-path overrides; privileged install never searches shared
/Documents or another user's cwd for dependencies. Extracted installer engines
run from the initiating administrator's private Temp with the same protections
against other users. Respect F5 builtin-store protection. Test replacement,
ancestor rename, LFN/8.3 aliases, shortcut retargeting and adjacent/current-directory
DLL planting, both before and after reboot. Do not make all shared Documents
read-only or rely solely on build-time hash verification.

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
name, uid and primary gid from LOOKUP, then canonical profile/home and supported
shell from authorized PROFILE (section 4.1); no password verifier. Own/admin
queries succeed; another user's private mapping returns EACCES in this subset.
If newlib has a pw_passwd extension it is `*`, not a hash. Unknown authorized user
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

Keep unified DOS roles exactly: **C: private writable FAT16, D: read-only
system view, E: removable view** (unified contract's redirected-drive matrix;
`dos_file_vm.c:189-207`). C: is never the managed host system disk. Persist its
COW/saves under the owner's protected profile, with a separate VM identity and
no cross-user overlay reuse. Private C: work/TEMP are short DOS paths such as
`C:\WORK` and `C:\TEMP`, created inside that provider; they do not point into
another user's host tree. Host-profile access, when exported through D:, is
separately credential-filtered and read-only: use stable DOS 8.3 aliases from the
existing resolver for accessible entries, never expose hidden/private names to
invent an alias. Writeable host-profile export, if later needed, requires a
separate explicit mapping contract; no new drive remapping/SUBST is added here.
E: file access remains constrained by both media/drive writability and credential
policy (Guest read-only); security does not silently enable a currently read-only
provider. No local-FAT fallback on denial and no DOS memory-limit change.

Preserve INT25/26 rejection on all managed drives (CF=1 AX=0201, extra FLAGS
stack convention, no transfer), INT21/7305 valid-packet rejection (CF=1 AX=020C,
no buffer change), and the existing invalid-parameter behavior. BIOS INT13 is
restricted to the private logical provider only, never D:/E:/physical sectors,
including administrator VMs. H06 tests short-path C:\WORK/C:\TEMP extraction,
FCB/handle/LFN paths, private-C persistence for Alice versus Bob, read-only D:
profile aliases, absent/read-only E:, denied-sector buffer/stack invariants and
zero physical callbacks for forged drive/provider/DFV2 requests.

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
| 1. **sec-k01 — credential and session foundation** | This reconciled freeze | `src/kernel/include/ciuki/{abi,process}.h`, `proc/{process,spawn,supervisor,syscalls_proc}.c`, common syscall dispatch, minimal Win32 child inheritance, channel/service retained-subject primitives, libciuki/newlib query wrappers, ABI oracle/SDK copy | Immutable credentials before process/thread publication; fourteen call-101 operations, six exact record layouts and role/service registrations; explicit SESSION_LAUNCH/LAUNCH_POLL and SECURE_ENTER/LEAVE with host grant adapters; gid 0, invalid uid sentinel and monotonic session ids; public spawn cannot elevate; protected service bootstrap and cross-credential fd sanitization. Allocation/copy failure exposes no child/session; peer fd reuse/forged uid fails. **H01**, ABI oracle and API-reference regeneration; existing kernel process/desktop regressions. |
| 2A. **sec-k02 — managed-volume policy and owner service** | sec-k01 | `src/kernel/fs/` policy/owner store and provisioning channel, resolved-object labels, `proc/posixpath.c` adapter; keep broad syscall enforcement for k03 | 32 rules/32 profiles/256 creator-owned entries per volume; Standard creates root files/folders with owner control and other-user read/execute; Guest read-only; aliases/LFN/8.3/dotdot/mount generations agree. One full-vector batch PREPARE/COMMIT/ABORT/STATUS, exact codec/receipts, OWNER-issued CREATE identities, shared manifest validation, receipt RESERVE/CHECKPOINT/COMPACT and nonreused G/keys/floors; every commit-order/power-cut/capacity fixture preserves ownership/retired ids. Three-account bootstrap and 18-binding (sixteen imported profiles plus two built-ins) migration are mandatory. Warm label lookup bounded. **H02**, **RFS**. Concurrent with sec-user-01. |
| 2B. **sec-user-01 — SYSTEM account service/store** | sec-k01; real publication additionally requires sec-k02 | New sealed account/logon service, private account IPC, v2 codec/KDF/filtering, `apps/desktop/users*` migration adapter; no ordinary desktop gate yet | 18-account schema and exact ready/Setup seed/state codecs, entropy-backed SID bootstrap/consumption, 8-63-byte policy; blank CiukiUser only as sole enabled account, irreversible retirement and password-first prerequisites, 10,000 KDF count, one KDF + one pending request/1000 ms retry; valid identity/last-admin/active EBUSY rules; durable reservation, stale generation/crash-lock recovery and no acknowledged-password rollback. Complete challenge relay/ENROLL/AUTOLOGON and DESKTOP lock route; 312-byte management, durable caller-owned operation-id RESERVE/STATUS, authorized PROFILE and metadata-only commits without OWNER; role endpoints/framing/best-effort cancellation, 160+128*N batch codec and 96+32*N replies agree with k02; three-account and sixteen-profile migration fixtures inject every ordering and abort/retry. Tests use the k02 owner adapter; live publication waits for k02. H03 also tests Guest/500/ordinary enable, blank rename/demotion refusal, password-first replacement, retirement, deletion of other users and reboot/service-restart nonrestoration; H01 covers endpoint/role spoofing, first-dequeue cancellation and launch/secure epochs; H03 covers failure correlation, both enrollment outcomes, reconnect STATUS, expiry and repeated self-updates beyond both receipt-window bounds. **H03**, **RUS**; root/owner integration after k02. Concurrent with k02, no writes to its kernel files. |
| 3A. **sec-k03 — all file/handle/mapping enforcement** | sec-k02 | `proc/{syscalls_file,fdtable,posixpath}.c`, VFS read/write/metadata paths and workers; Win32 file/section/view/loader sources only | Every native and lower-level entry point checks retained subject; cached exec, truncation, O_TRUNC, rename endpoints, root-owner rename/delete, directory slot reuse, attrs/times, mappings and deferred delete cannot bypass policy. Denied spy backend issues no private reads/writes. Cross-root move fences/revokes or returns EBUSY unchanged. **H04**, **RFS**, **RK**. Concurrent with k04 after k02; device dispatch edits belong to k04 and are integrated serially. |
| 3B. **sec-k04 — device and administrative authority** | sec-k02 | `drivers/{disk_dev,media_dev}`, `fs/disklease`, media/recovery/time/network/power and device dispatch; no generic file-policy edits | Every inventory action classified; remove executable-path role grants. Deny admin raw reads/writes on managed system disk; SYSTEM maintenance after admin check; retain exclusive leases/fencing. Guest read-only removable, local power allowed, session input/mixer/display distinct from machine defaults. Audit INPUT_KEYBOARD_SET and CIUKI_SETUP_NET_QUERY production dispatch; reject unsupported routes until repaired. **H05**, **RNET**, **RK**. Concurrent with k03, then user-02 may start. |
| 4A. **sec-k05 — DOS and per-user saves** | sec-k03, sec-k04 | `src/kernel/vm/dos*`, DOSVM client/save/provider paths, DOS launcher adapters | VM/workers inherit immutable uid/session; C: private writable FAT16, D: read-only system view, E: removable; INT21 handle/FCB/LFN/EXEC/metadata paths enforce VFS denial (DOS 5, no local-FAT fallback). INT25/26 AX=0201 and 7305 AX=020C remain failures with unchanged buffers/stack; BIOS INT13 only private provider, no physical callback. H06 includes private C:\WORK/C:\TEMP short paths, filtered D: 8.3 aliases and two-user COW persistence. Separate COW/saves/temp/print/clipboard per user; own reset/export and admin machine uninstall. No memory cap change. **H06**, **RK**. Concurrent with k06/user-02 in separate files. |
| 4B. **sec-k06 — Win32 isolation and registry broker** | sec-k03 | `proc/win32/` object/service/token/message/APC/snapshot/diagnostics paths (file/view/loader changes from k03 already integrated); `apps/w32srv` persistence/registry/restart and new SYSTEM broker, `sdk/libreg/registry.c` transport adapter and native machine-registry consumers | Real query-token handles and kernel credential facade, one user w32srv/session, SYSTEM-only raw authoritative hives; HKCU/HKU/.DEFAULT and HKLM checks on every operation. Libreg reads/generation polling/import/export/errors use production transport with backing access denied; personal preference backend stays owner-writable. Separate personal/machine schema-2 restart journals, v1 quarantine, admin admission and same-uid replay; H07 interrupts Alice, logs in Bob, then covers demotion/deletion/crash/uncertain launch. Cross-session objects, handles, APCs/messages/pixels/crash data blocked; self-memory restrictions preserved. Direct peer or identical registered session subject only, no forged forwarding. **H07**, **RW32**, **RK**. Concurrent with k05/user-02. |
| 4C. **sec-user-02 — Setup, ready images, migration and repair** | sec-user-01, sec-k02, sec-k04 | `apps/setup/`, `sdk/libsetup/`, `scripts/build_image.py`, `scripts/q3a_demo.py`, `config/cd-components.json`/shared Setup payload metadata, firstboot adapter, enrollment and confirmed CD repair | Exact section 3.6 Setup/ready records, allowed field combinations and persistent bootstrap/SID/consumed states; H08 injects every state barrier and missing/contradictory evidence. Custom uid 1000 and passworded disabled 500 on Setup, no CiukiUser; demonstrably unprovisioned ready seed alone creates blank CiukiUser once and leaves 500 unset. Generate machine SID before publication; no seed replay after loss. Unattended missing secrets fail before writes; no secret transport in scope. Restartable explicit G32 enrollment/maps, shared /Documents preserved, old verifiers protected. Confirmed lost-password CD repair is bounded and restartable. Move Q3ADemo.exe and any dynamically found shared executable into /Programs/Installers, protected desktop shortcut/ancestors/cwd and DLL planting tests; update payload-map/catalogue checks. No pre-Install ATA writes or plaintext log/state output. **H08**, **RSET**, **RLS**, **RUS**. Concurrent with k05/k06; builder changes are source work only in this later directive. |
| 5. **sec-k07 — trusted gate and kernel teardown integration** | sec-k03, sec-k04, sec-k05, sec-k06, sec-user-01 | Kernel secure-attention/input/display/grants and session drain; real-backend integration of k01 launch/secure-console operations | H09 qualifies SESSION_LAUNCH/readiness/failure rollback and SECURE_ENTER/LEAVE, one-use cookies, descriptor/process/service generations and role-restart crashes while apps stay alive concealed. Preserve authentication_required across ATTENTION; test pending launch->secure entry->ready/POLL, LOCK->Ctrl+Alt+Delete->Cancel, and service restart between each transition with stale success/LEAVE. Kernel consumes trusted physical Ctrl+Alt+Delete; secure UI revokes user input/fullscreen/framebuffer access including mapped and accelerated paths. Logoff blocks fresh work, stops all orphans/threads/Win32/DOS, drains admitted I/O, revokes handles/views/clipboard/replies. Failure to fence blocks next logon without panic. Session 0 survives; stale authentication/service replies cannot resurrect a desktop. **H09**, **RK**, **RWM**. No changes to ordinary logon/app UX owned by user-03. |
| 6A. **sec-user-03 — authenticated logon and session UX** | sec-k07, sec-user-02 | Trusted account-service UI, `apps/desktop/logon.inc`, profile/session startup and veto-aware Log Off | Enabled Welcome/typed names only, Safe Mode 500 password exception, unset 500 refusal, disabled Guest refusal; Cancel/Close never starts desktop. Ready autologon only sole-account/unretired tuple, never after Log Off/Lock/Safe Mode; H10 repeats Guest/500/ordinary enable, blank rename/demotion and reboot nonrestoration after every retirement transition. No SYSTEM fd/environment in desktop; profile/hive/startup failure retains secure error UI. Veto retains identity; unlock restores same session; passwordless CiukiUser gets set-password guidance. **H10**, **RUS**, **RWM**. Concurrent with user-05 after shared service interfaces are integrated. |
| 6B. **sec-user-05 — application-facing identity and compatibility** | sec-k05, sec-k06, sec-user-01, sec-user-02 | `sdk/win32/advapi32`/shell token, SID, username, CSIDL and privilege facades; native account lookup/authorized PROFILE/libc adapters; DOS launcher profile consumers; `sdk/win32/kernel32/console.c`, `services/start.c` console/default environment, internal log/temp consumers | Exact frozen read-only token/privilege view, SID/name short-buffer errors and truthful groups; PROFILE root/home/shell for a migrated uid with a different numeric suffix and denied foreign queries; direct authoritative kernel queries, no authority from client SIDs. HKCU, HOME/TEMP/CSIDL and per-user saves agree; validated generated TZ/LC_ALL/profile settings match k01 allowlist. H11 runs Standard automatic/explicit console with global TEMP denied, isolated logs, cleanup failures and original installer extraction/child launch; original installer metadata cannot elevate. Standard profile or creator-root install succeeds when supported, protected all-users install denies; F5 builtin protections persist. **H11**, **RW32**, **RUS**, relevant native ABI oracle if public headers change. Concurrent with user-03, no edits to its gate. |
| 7. **sec-user-04 — Control Panel, saver and resume** | sec-user-03, sec-user-05 | `apps/control/users.inc` and catalogue aliases/machine pages, desktop personal settings, saver/power hooks | User Accounts merges Users/Passwords with old launch alias, truthful admin/Standard/Guest controls; apply-time broker checks; last-admin/active EBUSY rules; session/machine saves separated. Guest self-password follows same old-password rule. Trusted saver dismissal and resume lock arm before restoring display/input; preserve unsupported standby/DPMS limits and exact Ciuki assets. **H12**, **RUS**, **RWM**, existing saver/power/control harnesses. |
| 8. **sec-k08 — coverage and bounded-cost audit** | sec-k01..sec-k07, sec-user-02, sec-user-04, sec-user-05 | Source-derived syscall/action policy census, cross-frontend adversarial harness and performance/ledger validation document | Every published selector has one policy/test mapping; unknown selectors deny. Warm fd checks add no storage lookup; measure metadata at 32 profiles/256 roots per volume, group/session/process limits and every final-reference failure. No secret/fd/pin/page/VM leak after successful teardown. KDF PIII/T23 measurements remain a lead runtime gate. **H13**, **RK**, **RFS**, **RW32**, complete security host harness. |
| 9. **sec-user-06 — integration and runtime evidence preparation** | sec-k08, sec-user-01..sec-user-05 | Documentation/inventory, runner-only security scenarios and private-fixture adapters | Wire SU-01..14 plus round-1 and round-2 challenge/lock/epoch/STATUS/checkpoint/profile, transition/reboot, launch/roles/batch/seed, protected-installer/DLL, DOS short-path, restart and console/libreg cases below as runtime cases without build variants. Record all host commands/results; reconcile API docs and supersession pointers. Inventory stays partial until its evidence exists. **H14** (host parser/fixture wiring only), all relevant regressions serially. Canonical build, authorized QEMU and hardware are the lead's later work. |

Round-2 acceptance additions are mandatory alongside the original rows:

| Decision | Harness owners / required cases |
| --- | --- |
| A2-1 | H01/H03/H10: UID and typed-name BEGIN_AUTH, two UI links with different generations, forged cross-connection generation, unknown name, changed target/action, failed outcome correlated to the right attempt, replay/cancel/reconnect/service restart; ENROLL_PLAN exact/changed retries, staged/final/failed ENROLL without session creation, short imported password replacement, and LOGON-only ready tuple autologon. |
| A2-2 | H01/H09/H10: same-session DESKTOP SESSION_LOCK succeeds; ordinary app, W32SRV, foreign/reused session/fd and synthetic LOGON_UI identity fail; no lock request can unlock or return a menu. |
| A2-3 | H01/H09: pending launch -> secure entry -> readiness/POLL; LOCK -> physical Ctrl+Alt+Delete -> Cancel; repeat with LOGON/LOGON_UI/ACCOUNT restart between each event and between success/LEAVE. Authentication-required stays set, epochs/cookies reject stale release, applications progress concealed, fresh owner authentication alone restores them. |
| A2-4 | H01/H03: cancellation before/after first server dequeue, before/after COMMIT_REQUIRED/OWNER COMMIT, lost reply, same-uid reconnect STATUS and exact retry, wrong-uid STATUS, expired ids and request-digest mismatch. Never infer rollback from result=0; no duplicate password change/account/owner allocation. |
| A2-5 | H02/H03/H08: zero CREATE locator, OWNER re-resolution/reservation after restart, spoofed stat inode/slot, changed manifest byte/tree/hash/header/map, alias/root/ancestor replacement and a sixteen-profile relocation. Both adapters read the identical destination staging file and deny on mismatch. |
| A2-6 | H02/H03: >2048 metadata changes with interleaved password changes do not grow OWNER log; >2048 RESERVED->ABORTED ownership batches (no committed profile occupancy) exercise receipt-window turnover; a small CREATE/RETIRE set separately verifies permanent retained-root capacity. Inject cuts before/after every checkpoint marker/trim; RESERVED->ABORTED before PREPARE counts once, compacted key/id cannot execute, active intents survive, and healthy compaction permits later management/passwords. |
| A2-7 | H03/H10/H12: Guest/500/ordinary enable before CiukiUser password fails; disabled account staging is allowed; password-first ordinary replacement obeys active-account EBUSY and last-admin at each step. Reboot/service restart never restores autologon. No atomic replacement codec exists. |
| A2-8 | H03/H11: uid 1007 with retained root u00000003 and another owner's u000003ef; PROFILE/home/shell and native adapters use the retained mapping, foreign Standard/Guest query denies, administrator/LOGON query authorizes, stale/retired/corrupt mapping fails without path truncation/leak. |

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

Round-1 additions to the same SU cases (not permission to run them here):

| Cases | Additional pass criterion |
| --- | --- |
| SU-01/04/12 | Fresh sole blank account autologons; enabling Bob, Guest or normal 500 while it stays blank/enabled fails before write. Only password-first replacement succeeds: set CiukiUser password, create/enable another administrator, log off, then disable/demote CiukiUser. Disabling the sole blank administrator and any atomic replacement request fail. Blank rename/demotion fails; disabled blank cannot be reenabled without password. Reboot after password, rename, demotion, disable/replacement and after deleting other accounts never restores retired autologon. Explicit Log Off still suppresses the fresh exception for that boot. |
| SU-07/09/11/13 | Spoof all registered service roles, replay old endpoint/process generations, cancel during fragmented request/KDF/commit; no elevation or late reply. Inject desktop/w32srv readiness timeout/negative ack/crash; trusted gate remains. Lock keeps apps running, service crash conceals them, only fresh owner authentication/cookie restores them. |
| SU-05/08 | Replace Q3ADemo.exe, rename store or presentation ancestors, retarget bundled shortcut, plant adjacent/current-directory DLL; Standard/Guest attempts fail and administrator installer runs the original protected payload from trusted cwd. Verify console-subsystem automatic backing and installer temp extraction with Windows/TEMP denied. |
| SU-05/07 | DOS C:\WORK/C:\TEMP remain private/short; D: filtered profile aliases read-only, E: provider permissions unchanged. INT25/26/7305 return exact unified errors without transfer; BIOS INT13 cannot escape private provider. |
| SU-07/13 | Alice's interrupted personal restart queue stays suspended through Bob's logon; machine journal replay needs broker authorization. Demotion/deletion, unowned v1/.DEFAULT migration and uncertain program launch never execute as Bob or SYSTEM. Existing native applets use production libreg adapter with direct machine backing access denied. |
| SU-02/13 | Bootstrap has three account/owner bindings; migration has all sixteen legacy profiles plus built-ins. Interrupt each PREPARE/COMMIT/DB/seed/SID state barrier; STATUS/key/digest/watermarks agree across adapters. Abort consumes generations, missing/contradictory consumed state never creates blank CiukiUser. |

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

Binding sources: [sec-lead-decisions.md](../directives/sec-lead-decisions.md) and
[Lead amendment 1](../directives/sec-contract.md#lead-amendment-1-2026-10-26-1145-cross-review-round-1) and
[Lead amendment 2](../directives/sec-contract.md#lead-amendment-2-2026-10-26-1245-cross-review-round-2).
A1-n/A2-n identify those amendments' numbered decisions; A2 takes precedence.
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
| K-Q2 | SYS_SECURITY=101 is frozen in the kernel half with six layouts, fourteen operations, peer fd, profile descriptor, errors/flags/bounds and the read-only SID/token facade. W32_DISPLAY remains under call 96. |
| K-Q3 | 32 policy rules, 32 profile roots, 256 creator-owned root files/folders per volume. Additional fixed volumes follow the same root rule as data volumes, never unprotected system aliases. |
| K-Q4 | Sealed SYSTEM-only boot/service set; all user raw reads/writes on managed system disk refused pending exact sector classification. Maintenance/Setup acts through SYSTEM after admin checks. Common prefix policy replaces Win32 all-/System denial; F5 builtin-store protections remain. |
| K-Q5 | Per-session user w32srv plus SYSTEM machine-registry broker; no SYSTEM desktop. All authoritative hive backing files service-only; HKCU/HKU/.DEFAULT correctly bound; personal non-authoritative libreg preferences may remain owner-writable. |
| K-Q6 | Exact XP default parity is unnecessary; document CiukiOS choices. No persistent lockout, one-second bounded retry. Historical gaps are not implementation blockers. |
| K-Q7 | Ready 500 unset, 8-63-byte custom/Guest passwords, and explicit Setup CD account repair if all administrator credentials/verifiers are lost or corrupt. No implicit empty recovery. |
| K-Q8 | Local shutdown/restart for all interactive users including Guest and on logon screen; mixer/display/input preferences per session, machine defaults admin-only. Sec-k04 audits INPUT_KEYBOARD_SET and CIUKI_SETUP_NET_QUERY and refuses unsupported routes until fixed. |
| A1-1 | Sole-enabled-account blank CiukiUser; A2-7 supersedes password-or-disable/atomic replacement with password-first ordinary operations before another enable/blank rename/demotion. Monotonic auto_logon_retired and reboot/transition tests (main 3.1/3.3/4.2, H03/H10). |
| A1-2 | Call-101 SESSION_LAUNCH/LAUNCH_POLL/SECURE_ENTER/LEAVE require kernel-established SYSTEM LOGON role; exact purpose/descriptor/generation/readiness/rollback/crash contract, apps alive while locked (kernel ABI, H01/H09). |
| A1-3 | Fixed bootstrap roles and service ids, CONNECT/ACCEPT/CANCEL, noninheritable retained peers and bounded framed requests; no claimed-name/uid authority (kernel ABI, main 4.1, H01/H03). |
| A1-4 | One full-vector owner/account batch, one G and reserved owner generation, exact request/reply, durable receipts/STATUS, nonreuse and every commit-order recovery; three-account and sixteen-profile fixtures (main 3.5, H02/H03/H08). |
| A1-5 | Quake III bundled installer and any other shared executable move to protected /Programs/Installers; protected desktop reference/ancestors/cwd, DLL planting coverage; build/catalogue change belongs to sec-user-02 (main 8.3, H08). |
| A1-6 | Exact ready/Setup seed records and persistent bootstrap/SID/consumed codec/state table; only proven unprovisioned origin may initiate blank provisioning; every interruption closes or resumes the same transaction (main 3.6, H03/H08). |
| A1-7 | DOS C: private writable FAT16, D: read-only system view, E: removable; private short work/TEMP and per-user COW separate from filtered profile export; exact INT25/26/7305 failures and private-provider BIOS INT13 (main 8.4, H06). |
| A1-8 | Separate owned personal/machine restart journals, v1 quarantine, current same-uid replay and broker-authorized machine work; logoff/demotion/deletion/crash and Alice-then-Bob cases (main 6.2, sec-k06/H07). |
| A1-9 | sec-k06 owns native libreg reads/generation/import/export/errors adapter, preserves personal preference backend and tests production applets with backing access denied (main 6.2, H07). |
| A1-10 | sec-user-05 migrates console backing/startup, internal logs and temp consumers; generated validated TZ/LC_ALL/profile settings reconcile k01 allowlist (main 8.2, kernel ABI, H01/H11). |
| A1-11 | Remove unsupported XP Home Safe Mode attribution; M-Q1 remains the explicit CiukiOS decision, KB814777's Server/RIS scope is identified (main R4, kernel R9). |

| A2-1 | Main 4.1 freezes selector-bearing BEGIN_AUTH, LOGON-originated CHALLENGE/verification with kernel-observed UI endpoint generation, correlated success/failure outcomes, ENROLL_PLAN/staged/final ENROLL and retained-ready-tuple-only AUTOLOGON. AUTH verification trusts LOGON role only (H01/H03/H10). |
| A2-2 | LOGON service SESSION_LOCK=5 accepts only kernel-established same-session DESKTOP and can request only lock; no unlock/menu-return/authentication authority (kernel service table, main 4.1, H01/H09/H10). |
| A2-3 | Persistent session authentication_required plus console/authentication epochs; ATTENTION cannot lower a lock, POLL/readiness cannot release a newer gate, service death preserves concealment and invalidates old success/cookie (kernel transition table, H01/H09). |
| A2-4 | SERVICE_CANCEL proves cancellation only before first server dequeue; otherwise irrevocable-or-unknown. ACCOUNTS RESERVE/STATUS binds durable client operation ids to original uid/content across reconnection, with bounded expiry/nonreexecution (main 4.1, H01/H03). |
| A2-5 | All request locators zero; OWNER resolves/reserves CREATE roots and existing owner keys itself. Both sides independently validate identical protected destination manifest/tree/map evidence before publication; no stat-derived identity (main 3.5, H02/H03/H08). |
| A2-6 | Metadata/password/unchanged-binding updates use ACCOUNT journal only. OWNER RESERVE includes early ABORTED capacity; CHECKPOINT/COMPACT preserves maps/watermarks/floors with bounded terminal windows and crash recovery, no lifetime management limit (main 3.5/4.1, H02/H03). |
| A2-7 | CiukiUser receives a password before any other interactive account may be enabled. Replacement uses ordinary create/enable then logoff/disable/demote with last-admin protection, no atomic replacement (main 3.1/4.2, H03/H10/H12). |
| A2-8 | Authorized ACCOUNTS PROFILE=8 returns canonical committed root, home and supported shell without uid-derived spelling; closes sec-k01 report question 3 through a service query, no extra syscall (main 4.1/8.4, H03/H11). |

No new policy question remains open in this reconciliation. Lead cross-review,
implementation, target KDF measurements, runtime security/installer evidence and
hardware qualification remain outstanding. The proposed security QEMU suite
still needs the lead's existing policy authorization before execution; this
document grants none. No runtime evidence is claimed by the design.


## Delta for sec-k01 and sec-user-01

Relative to committed reconciliation **3ddc9de88** (round-1 additions, read with
the round-2 superseding rows below), these are interface changes
for the implementations already in progress; the lead must amend both directives
before treating their old interface freeze as current. They are design changes,
not delivered headers/code or completed tests.

| Owner | Required delta |
| --- | --- |
| sec-k01 | Keep SECURITY=101, operation values 1..7 and existing credential/profile/args 64/112/64-byte fields unchanged. Add operations 8 SESSION_LAUNCH, 9 LAUNCH_POLL, 10 SECURE_ENTER, 11 SECURE_LEAVE, 12 SERVICE_CONNECT, 13 SERVICE_ACCEPT, 14 SERVICE_CANCEL; add launch/console/link records of 96/64/64 bytes with every literal field/offset/constant from kernel ABI. Reuse args.spawn_address for the operation-specific input and disjoint output buffers. QUERY_PEER args.result now returns endpoint generation; SESSION_QUERY args.result returns current session generation (freed DEAD=0), replacing constant zero. |
| sec-k01 | SESSION_CREATE/SPAWN_CREDENTIAL/SESSION_END and all launch/secure transitions require uid 0 **and registered LOGON role**, replacing uid-0-only authority. Bootstrap fixed LOGON/LOGON_UI/ACCOUNT/OWNER/REGISTRY/SETUP roles; session launch assigns DESKTOP/W32SRV only. Freeze fixed image paths, role/service ids, six service endpoints and generation/noninheritability/no-transfer rules; public copies, descendants and exec never acquire roles. |
| sec-k01 | Implement purpose-specific private child preparation, safe stdio/console, role/endpoint/grant assignment before publication, paired ready handshake and 5000 ms timeout; explicit atomic console handoff, one-use restoration cookie, all generations/rollback/service-crash rules. Secure entry keeps application execution alive while presentation/input are concealed. Supply the host-testable grant adapter now; sec-k07 still qualifies real backends. |
| sec-k01 | Add role-bound CONNECT/ACCEPT/CANCEL and retained-subject acquire/get/put/check primitives. Reuse channel/Win32 accepted-link machinery without reinterpreting existing Win32 ids 1..2. Framing is 64-byte header + <=192-byte fragments, <=8192-byte logical body, one exchange/link and eight total, no attachments; close/death/cancel revokes queued subjects/replies. All six public records and new constants enter the ABI oracle, with one field per header line, ABI run and generated API reference in the later code task. |
| sec-k01 + sec-user-01 | Cross-credential environment now also admits generated LC_ALL=C, validated committed TZ, generated CIUKI_PROFILE_SETTINGS and purpose-specific descriptor labels. Never inherit SYSTEM environment/settings/stdin. Auth outcomes bind one consumed attempt to actual UI endpoint/console/account generations; ACCOUNT reports to LOGON, which alone creates/launches/unlocks the session. Main 4.1 freezes endpoint operation/body codecs. |
| sec-user-01 | Add accounts.db header auto_logon_retired bit 2, propagated monotonically into bootstrap.state. Enforce sole-enabled-account blank tuple, password-first before other enable/blank rename/demotion, ordinary replacement (A2-7), no blank reenable or reboot restoration; exact configured/unset/imported/blank field combinations in 3.3. H03/H10 must include Guest/500/ordinary accounts and all reboot transitions. |
| sec-user-01 + sec-k02 adapter | Replace single-uid generation messages with main 3.5's full 160+128*N request and 96+32*N reply, key=(SID,volume,G), one batch per ownership-changing operation, one reserved G and owner generation, nonreuse after abort, bounded STATUS receipts with checkpoint floors (A2-6), destination staging and COMMIT_REQUIRED recovery. Include bootstrap's three bindings and migration's sixteen profiles plus built-ins, not a single-account fake. No authenticated publication before real k02. |
| sec-user-01 | Implement exact CIUKISEE ready/Setup records, including seed-only ready flags=9/KDF=0 and published flags=13/fresh empty verifier, and 160-byte CIUKIBST persistent bootstrap codec/table. Only matching UNPROVISIONED evidence may begin a blank recipe; SID_READY resumes sealed staging, CONSUMED never replays. Fail closed for missing/contradictory evidence at each interruption. Share codecs/fixtures with sec-user-02. |

Later source ownership remains explicit: sec-user-02 moves bundled executables and
updates build/catalogues; sec-k05 preserves DOS drive/sector/short-path semantics;
sec-k06 migrates libreg and owned restart journals; sec-user-05 migrates console,
logs/TEMP and identity/profile consumers. Their named H01..H14/SU additions are
future acceptance work; no host tests run for this amendment.

### Review round 2: delta relative to f3b388722

All source/header/test changes below belong to subsequent implementation; this
amendment edits documentation only. Keep SYS_SECURITY=101, operations 1..14,
all credential/profile/args/link layouts and existing launch/console sizes.

| Owner | Required delta from f3b388722 |
| --- | --- |
| sec-k01 | launch[96]: replace reserved[4]@80 with activation_console_epoch@80, activation_authentication_epoch@84, reserved[2]@88. console[64]: replace reserved[6]@40 with authentication_required@40, console_epoch@44, authentication_epoch@48, reserved[3]@52. Each new field is u32; sizes/alignment unchanged. Include the added fields/offsets in the later ABI oracle/header/API-reference work. |
| sec-k01 | Implement kernel-retained authentication_required and both epochs/initial activation guard; SECURE_LEAVE checks exact current epochs/bit plus cookie. SECURE_ENTER(session 0, ATTENTION) supplies the trusted pre-session binding; session-0 LEAVE is denied. Initial readiness/POLL cannot release a superseding gate. Preserve required bit across ATTENTION and service restart; H01/H09 sequences are mandatory. |
| sec-k01 | Service AUTH permits LOGON-originated verification/challenge/enrollment/autologon, UI LIST only. LOGON permits same-session DESKTOP SESSION_LOCK only in addition to its trusted clients; kernel peer role/session/generation remains authoritative. Supply retained role/session checks for the main 4.1 per-operation service allowlists, enforced by ACCOUNT/LOGON servers in sec-user-01; CONNECT alone is insufficient. No kernel inspection of password payloads is required. |
| sec-k01 | SERVICE_CANCEL result=1 only with proof no request fragment reached server and queue removal; result=0 is irrevocable-or-unknown. First successful server receive/copy is the transport admission boundary. No durable-state/completion notification syscall is added. Revoke old replies while already admitted work uses its retained subject/durable recovery rules. |
| sec-user-01 | Replace 32-byte BEGIN_AUTH with 64-byte account-selector request and freeze the 64-byte challenge, 128-byte CHALLENGE request, 272-byte LOGON verification relay, 304-byte correlated AUTH_OUTCOME including failures and 16-byte public progress. LOGON forwards the kernel-reported UI->LOGON generation in requests it originates; ACCOUNT trusts that LOGON role, not a copied UI claim or its own endpoint generation. Add ENROLL_PLAN=7 relay and staged/final ENROLL; no ENROLL outcome creates a session. Add AUTH AUTOLOGON=6 only for the retained ready tuple/per-boot latch and initial LOGON/ACCOUNT role generations=1; either role restart permanently suppresses it for that boot. |
| sec-user-01 | LOGON SESSION_LOCK=5 accepts own DESKTOP only and cannot unlock. Bind every outcome to current session/console/authentication epoch/account and service generations. Late failures/successes and restarts cannot consume another attempt or clear a newer lock. |
| sec-user-01 | Management body becomes 312 bytes with client_operation_id@304. Add ACCOUNTS STATUS=7, PROFILE=8, RESERVE=9 and exact codecs/authorization; reserve a nonreused durable client id before mutation, persist it before send, same-id retries never reexecute. Bounded account result checkpoints return EXPIRED after retention, never imply rollback. Ownership-neutral metadata/password changes use only ACCOUNT journal. |
| sec-user-01 + sec-k02 adapter | Locator@32 is zero for all request bindings; OWNER alone resolves/reserves roots and existing keys. Shared CIUKISTG manifest/header/entries/tree hash and original G32 mapping are validated by both sides. Add OWNER RESERVE=7 (state RESERVED=5), CHECKPOINT=5 and COMPACT=6 (CHECKPOINTED=4) with reserved ABORTED capacity, coherent snapshot/marker/floor publication and cut recovery. Old receipts compact rather than lasting forever; old keys cannot execute. |
| sec-user-01 | Replace the password-or-disable/atomic replacement alternative with password-first only. Last-admin and active-account checks apply after each ordinary create/enable/disable/demote; no new replacement operation. Enabling Guest/500/any account while CiukiUser stays blank fails. |
| sec-user-01 + sec-user-05 | PROFILE response[272] returns committed root/home/shell and both generations with own/admin/LOGON filtering; native getpw* and environment adapters must not infer roots from uid. Test retained G32 root whose numeric suffix differs. This answers sec-k01 report question 3 through service transport. |

The lead must propagate this delta to the active sec-k01/sec-user-01 directives
and the sec-k02 adapter before integrating implementations based on f3b388722.
No new policy decision is requested; implementation/review/runtime evidence
remains future work. H01..H14 are acceptance specifications, not tests run here.
