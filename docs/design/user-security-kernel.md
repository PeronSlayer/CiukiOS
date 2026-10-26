# User security: kernel enforcement contract

Implementer draft for lead cross-review, recorded 2026-10-26 after checking
the workstation date. Source baseline: 17fd29b2a4d09aa78ba9b1109b11e0f25b0e8b85,
branch wt/sec-kernel. Authority: [sec-kernel directive](../directives/sec-kernel.md).
This document specifies future implementation; it does not claim that the
current kernel enforces these rules. No source, ABI header or SDK changes
accompany it.

## Decisions and precedence

K1. The owner's real-user decision supersedes G32's profile-only security,
Cancel-to-default logon and the Windows 9x model for users/security only.
Native processes, Win32 processes and DOS VMs all operate under kernel-owned
credentials. There is no anonymous desktop when authentication is cancelled.
Drivers, scheduling, storage durability and the V86/DPMI architecture retain
their existing contracts.

The following shared decisions reproduce the lead's directive:

- Identities: 32-bit user ids. 0 = SYSTEM (kernel, boot services, the logon/account service); 500 = CiukiAdministrator; 501 = Guest; 1000 and up = ordinary accounts (CiukiUser or the Setup user is 1000). Ids are never reused.
- Groups (NT well-known RIDs as numbers): 544 Administrators, 545 Users, 546 Guests. Account types shown to the user: Administrator (544+545) and Standard/Limited user (545); Guest (546).
- Each process carries an immutable credential record {uid, group set, session id, flags}; children inherit it; only SYSTEM (the logon service) can create a process with a different credential. No setuid binaries. Administrators run with full administrator rights (Windows XP model, no UAC split token). A "Run as administrator" credential prompt may come later and is out of scope unless the contract finds it needed for installers.
- FAT32 has no ACLs (Windows XP on FAT32 has no file security at all); CiukiOS enforces a **kernel path policy** on its own volumes: rules by path prefix plus owner metadata for profile trees, stored in a SYSTEM-only place. Windows XP default NTFS permissions are the model for what Users/Administrators/Guests may do in the root, Windows/System, Program Files, Documents and Settings (Profiles) and settings directories.
- Account database: SYSTEM-only (path policy), salted PBKDF2-HMAC-SHA-256 verifiers as in G32 (re-tune iterations for a Pentium III if needed and say why), recoverable locking (no stale lock can block management).
- Physical access is out of the threat model (a FAT32 disk is readable from another OS); no disk encryption, no network authentication in this step.
- Existing per-binary "authority" mechanisms (`docs/design/execution-abi.md`, CIUKI_*_AUTHORITY, `--authority-fd`) are inputs: map each to the new model (admin-only, SYSTEM-only, or per-user) rather than keeping two parallel systems.

The binding account defaults are:

| Installation | Accounts and authentication |
| --- | --- |
| Ready-made image, including raw/QEMU/VirtualBox/VMware distribution | CiukiUser uid 1000, Administrators+Users, empty password, direct logon. This is an explicit authenticated empty-password account and autologon policy, not a Cancel bypass. |
| Setup installation | Mandatory custom user uid 1000, first administrator; no CiukiUser. Setup asks for the CiukiAdministrator password. |
| Both | CiukiAdministrator uid 500 is built-in, disabled and hidden normally; Safe Mode exposes its password-authenticated recovery logon, or an administrator explicitly enables it. Guest uid 501 exists, disabled by default. |

K2. Four authorization classes are used below: anyone, own-session,
administrator and SYSTEM. "Anyone" means any live process already admitted
by the kernel, including an authenticated Guest; it does not create a logon
route. "Own-session" additionally requires a live matching session and the
specific descriptor/process/VM ownership stated in the row. Sharing a uid
alone never grants control of another session. Administrator means uid 0 or
membership in 544; uid 500 has full administrator rights, not uid 0 rights.
SYSTEM means uid 0 executing a trusted boot/service operation. These are
authorization predicates, not four interchangeable tokens or a blanket
hierarchy overriding device ownership, read-only media or session isolation.

K3. Remove executable-name grants as a source of privilege. Identity decides
administrative rights. A display/input grant or accepted service endpoint
still identifies the actual owner of an exclusive resource; it cannot
increase the holder's credential. Protected service images are started by
SYSTEM with explicit roles. Public spawn of even the exact same path cannot
manufacture a role. No compatibility fallback may reinstate a former grant.

K4. Kernel enforcement is mandatory for all volume access. Native libc or
desktop disabling a button is not enforcement. Shared VFS entry points need
an explicit retained subject, and operations performed by workers retain
the originating subject. A missing subject on a user-reachable path denies
access; it must never silently become SYSTEM.

K5. One interactive console session is active at a time in this step.
There is no fast user switching, network logon, impersonation API, UAC,
arbitrary ACL editor, executable setuid bit or unrestricted chown/chmod.
Administrative installers can run after logging on with an administrator
account. A credential prompt is not required to make that path work.
Limited users may install into their writable profile; machine installs
must fail access checks rather than elevate automatically.

## Primary research and comparison

These sources were consulted on the contract date. General API pages describe
mechanisms; their modern defaults are not treated as XP installation defaults.
Each numbered conclusion below is followed by the CiukiOS decision it informs.

| Ref | Primary source and finding | Decision |
| --- | --- | --- |
| R1 | Microsoft [well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids) distinguishes account RIDs, builtin groups and logon identities. | Use the lead's numeric uid/group assignments. These numbers are not complete Windows SIDs and uid 0 is not RID 500. |
| R2 | Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens) associates user, groups and logon identity with processes; [privilege constants](https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants) distinguishes system privileges from object rights. | Immutable process credentials; separate resource ownership from administrator membership. No thread impersonation in this step. |
| R3 | Microsoft's [MS02-064 XP root baseline](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064) contrasts the Windows 2000 writable root with XP. XP permits Users to create root subdirectories; it does not grant unrestricted root file creation or overwrite. | Protected installed anchors plus creator-owned root data directories, rather than claiming that every XP root entry was read-only. |
| R4 | Microsoft [gaming with least-privileged accounts](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/gaming-with-least-privileged-user-accounts) gives XP NTFS folder defaults, FAT32's lack of security, read-only HKLM and per-user HKCU storage. | Protect OS/program paths, permit own profile data, provide explicit shared documents, and enforce the policy on FAT ourselves. Games patch machine installs as administrator. |
| R5 | Microsoft [registry key security](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights) checks requested key rights and inherits key security from the parent; [registry structure](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users) distinguishes HKLM, HKU and HKCU. | Authorize each registry operation against its authenticated client and namespace; protect entire hive backing files against bypass. |
| R6 | The Microsoft [Windows XP Security Guide, chapter 3](https://learn.microsoft.com/ko-kr/security-updates/security/20213971), retained in Korean with English policy names, assigns shutdown to Administrators+Users and clock/driver/volume maintenance to administrators in its client configurations. These are configuration recommendations, not proof of every XP default. | Allow local interactive shutdown/restart; reserve machine configuration and maintenance for administrators. |
| R7 | Microsoft [SetTimeZoneInformation](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-settimezoneinformation) explicitly requires SeSystemtimePrivilege on XP/2000; the separate time-zone privilege applies to later Windows. | Both system clock and system time zone require administrator. Per-process TZ conversion can remain unprivileged. |
| R8 | Microsoft [format/eject removable media policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/jj852171(v=ws.11)) includes XP and distinguishes use of media from format/eject rights; [media-burning device permissions](https://learn.microsoft.com/en-us/windows/win32/imapi/providing-user-permissions-for-media-burning-devices) identifies XP's extra device permissions. | Ordinary file use is per-session. Formatting/raw snapshots require administrator. Allow safe removal/tray use to the local session as an explicit CiukiOS usability choice. No media-burning permission is implied. |
| R9 | Microsoft [KB814777](https://learn.microsoft.com/en-us/troubleshoot/windows-client/windows-security/access-computer-after-administrator-disabled) documents Safe Mode access to a disabled local administrator in its server/RIS scenario. [Advisory 906574](https://learn.microsoft.com/en-us/security-updates/securityadvisories/2005/906574) explains XP Guest/ForceGuest network behavior. | Follow the owner's explicit CiukiAdministrator/Guest defaults, not an assertion that every XP edition disabled Administrator or that Guest visibility is equivalent to network authentication. |
| R10 | Microsoft [lockout threshold](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/hh994574(v=ws.11)) describes zero/no lockout and gives later client defaults; [MS-GPSB lockout policy](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpsb/2cd39c97-97cd-4859-a7b4-1229dad5f53d) defines zero as disabled. | Propose threshold zero plus bounded authentication work, with no permanent lockout by failed guesses. Exact XP template evidence remains Q6. |
| R11 | The Open Group [getuid, SUS Issue 6](https://pubs.opengroup.org/onlinepubs/009695399/functions/getuid.html) returns the calling process's real uid; [access](https://pubs.opengroup.org/onlinepubs/9699919799/functions/access.html) explains why prechecking a path cannot authorize a later operation. | Real/effective ids coincide; authorization belongs inside the actual operation. The getgroups reference was inaccessible (HTTP 403); its planned wrapper contract is specified below without claiming a new POSIX conformance level. |
| R12 | [RFC 8018 sections 4 and 5.2](https://www.rfc-editor.org/rfc/rfc8018.html) specifies salt/iteration parameters and PBKDF2. | Keep G32's algorithm and 10,000 iterations pending target measurement; do not lower cost based on a host benchmark. |

The owner's Windows 98 Resource Kit was read only at
/home/peronslayer/Desktop/CiukiOS/build/rskt: wrkc07_3.htm:10–54 describes
default/named profiles and User.dat; wrkc18_2.htm:10–24 distinguishes local
and network logon. It supplies G32 baseline context only. No book text or
private media is imported. DOS remains a V86/DPMI guest with VMM file
arbitration, not Windows NT NTVDM.

Research limits are explicit: no factory XP security-template dump was
available here. Exact HKU subkey exceptions, XP lockout defaults and
edition-specific Welcome-screen filtering are not inferred from a modern
Server page or community answers. The owner already fixes CiukiOS's logon
behavior. Q6 records the remaining historical evidence, independently of
the kernel's normative access matrix.

### Repository evidence

File:line references identify the inspected baseline, not future locations.
Read ranges were selected using rg and sed, with no Semble.

| Area | Current implementation and gap |
| --- | --- |
| Process identity | src/kernel/include/ciuki/process.h:35–87 carries pid/ppid/pgid, personality and capability booleans but no uid/groups/security session. src/kernel/proc/process.c:64–101 prepares a child; :126–154 inherits descriptors. A process group is not a security identity. |
| Trusted launch | src/kernel/proc/supervisor.c:136–170 installs mixer/tasks/service authority; :181–207 grants DOS/program/network/DHCP/time/recovery booleans by parent and exact path. Public spawn repeats delegation at src/kernel/proc/spawn.c:363–365. These are to be replaced, not supplemented with uid checks that leave the old escape route active. |
| Common syscall entry | src/kernel/core/syscall.c:142–200 routes native, DOS and Win32 operations. src/kernel/proc/syscalls_file.c:323–365 performs native pathname work; :367–505 routes devctl and fd work. Checking only call 29 misses DOS, Win32, existing descriptions and workers. |
| Native and legacy names | src/kernel/proc/posixpath.c:356–379 validates native names; :453–510 resolves entries and dot components. src/kernel/fs/vfs.c:1647–1662 resolves DOS-style volume paths; :1669 begins open/create. Both namespaces must use the same resolved policy identity. |
| Existing write restriction | src/kernel/proc/fdtable.c:99–105 blocks Win32 writes under C:/System by personality; :314–315 checks that and volume write state. It is neither per-user security nor sufficient protection for other frontends. |
| Raw disk | src/kernel/fs/disklease.c:221–246 admits a nonboot partition editor without a user-role test; :438–459 permits reads without authorization. src/kernel/drivers/disk_dev.c:10–13 passes pid/Setup flag only. Both are explicit security migration items. |
| Maintenance/media | src/kernel/drivers/media_dev.c:137–176 routes format/conversion/defrag from a /dev/media fd with no uid check. Recovery uses path-delegated recovery_capability at src/kernel/proc/syscalls_recovery.c:20–59. |
| Time/network/power | src/kernel/proc/timezone.c:288 checks time_capability; src/kernel/net/lwip/syscalls.c:227–250 checks network/DHCP capabilities. src/kernel/core/power.c:417–423 identifies desktop/Setup; :515–540 makes EVENTS public and POLICY owner-only, unlike the older contract's blanket restriction. |
| DOS | src/kernel/vm/dosvm_client.c:300–365 separates image/game/save queries from management; src/kernel/vm/dos_file_vm.c:99–106 attaches a VM table to native storage; :421–432 routes raw guest sectors through its private logical volume. Guest PSP/VM lock-owner ids are not user credentials. |
| Win32 | src/kernel/proc/win32/syscall.c:30–75 dispatches all families under global call 96. src/kernel/proc/win32/service.c:45–57 delegates service roles by exact path. src/kernel/proc/win32/system.c:21–26 restricts memory copying to self. These narrower checks stay narrow. |
| G32 | apps/desktop/users_io.c:61–75 stores users.dat and an exclusive lock file under /system/settings; :81–101 derives/verifies and saves it. apps/desktop/users.c:135–153 maps profiles to /Windows/Profiles/u%08x or the shared default. The desktop currently owns the verifiers; that responsibility must move to SYSTEM. |
| Native registry | docs/design/registry-contract.md:47–58 places all persistent roots under /System/Registry; :95–128 uses recoverable replacement but requires a single writer. A shared Users.reg or Desktop.reg cannot safely hold private writable user data. |
| Win32 registry | docs/design/win32-personality-contract.md:786–807 specifies one USER.CRG/HKCU=.DEFAULT. docs/design/execution-abi.md:2233–2245 authenticates a service peer PID. A username or uid in request bytes must never choose the effective hive. |
| Setup | docs/design/cd-setup-plan.md:336–384 supplies noninherited Setup leases, :759–777 Setup metadata authority, :798–807 Setup-only CD boot. Account provisioning must precede exposure of the first interactive desktop. |

Contract references read before source inspection:
[execution ABI](execution-abi.md),
[VFS/storage](vfs-storage-contract.md),
[unified DOS](unified-dos-contract.md),
[Win32 personality](win32-personality-contract.md),
[registry](registry-contract.md),
[users/logon](users-and-logon.md) and
[CD Setup](cd-setup-plan.md).
Their old security statements are superseded only as listed here. In
particular, the execution ABI at line 2315 says "Call 101" for delegated
display, while its header/actual dispatcher use W32_DISPLAY under call 96.
Use the implemented call 96; reserve no new security number from that typo.
The "90 reserved ENOSYS" prose at lines 2678–2683 is also superseded by the
implemented call-90 amendment at lines 2725–2733.

## Process credentials, sessions and trusted services

### Credential lifetime

Each process MUST retain one immutable credential value before publication:
uid, primary group, bounded sorted unique supplementary group set, security
session id and kernel-validated flags. Eight group slots are proposed;
group-set overflow is an error, never truncation. An ordinary administrator
has primary gid 545 and groups 544+545; standard users have 545; Guest has
primary/group 546. SYSTEM has uid/gid 0 and the explicit SYSTEM predicate.
Admin status is derived from group membership, never a caller-supplied flag.
The kernel must reject uid/group combinations reserved for builtin accounts
when creating ordinary logon sessions.

proc_prepare, public spawn, Win32 CreateProcess, loader helpers, DOS launch
workers and every native thread MUST receive the same originating credential.
The record may be held by reference internally; refcounts must not wrap.
An exec replacement retains the credential and session and drops service
roles/privileged descriptors unless SYSTEM explicitly re-establishes a trusted
service. The current native ABI has spawn but no public exec syscall; this
rule also applies to libc's implementation and any future exec entry.
Orphan adoption by PID 1 changes ppid, never credential or session.

Only SYSTEM may use the proposed SPAWN_CREDENTIAL operation. It prepares a
new unpublished child with the selected live session's immutable credential,
checks executable access as that child, sanitizes cwd/environment/stdio and
descriptor sources, reserves resources, then publishes atomically. It MUST
NOT rewrite an already running process's uid or give a live process a token
setter. Failure publishes neither a PID nor transferred handles. A normal
parent cannot supply alternate credentials in spawn flags, environment,
argv, PE parameters, guest memory, inherited descriptors or RPC data.

Cross-credential spawn MUST discard all caller descriptors by default,
including 0–2, cwd and service links. Only explicitly created safe stdio,
same-session application channels and copied nonsecret launch data may enter
the child. An account-service database fd, writable system file, raw-disk
lease, input/display grant or SYSTEM endpoint can never be inherited through
this path. Kernel bootstrap of a trusted service uses a distinct internal
operation, not a user-requested executable path.

Account role/password changes update the database for future logons.
Existing credentials remain immutable. Disabling/deleting an account or
removing administrator membership MUST make the account service end that
account's live session before reporting effective revocation; otherwise the
UI must report that logoff is pending. Password change alone need not revoke
the owner's active session. Only SYSTEM may add/remove groups in the database;
the service accepts such management requests only from an administrator.
A user changing their own password must authenticate the old password.

### Sessions and logoff

Session 0 is reserved for boot and SYSTEM services. A monotonically allocated
nonzero 32-bit id identifies each logon during a boot; it is unrelated to
PGID, VM generation or the joystick boot discriminator. It is never reused
during that boot. Exhaustion returns EAGAIN. Only one console session may be
ACTIVE; bounded provisional/closing records may coexist. Persistent uid
allocation and its high-water mark survive deletion/restart and fail with
ENOSPC at exhaustion, without wrapping or recycling ids.

The account service creates a session after checking account state and the
password, then starts the desktop with that session's credential. The logon
UI and account service are separate from that desktop. A child of an
administrator is an administrator automatically, including an installer.
A standard user's child remains standard even if named setup, control or
w32srv. Ready-image autologon uses the same session path with CiukiUser's
explicit empty-password verifier and persisted autologon choice.

Logoff retains G3's application save/veto protocol:

1. The desktop queries only applications in its session. A veto keeps the
   session ACTIVE; no user/profile binding changes yet.
2. After confirmation SYSTEM changes the session to CLOSING under the session
   lock. New spawn, service links and new user I/O for that session are refused.
   The kernel snapshots all session members, including detached/orphaned
   children, native threads, Win32 processes and DOS VMs.
3. Stop execution, revoke input/display/presentation leases, destroy queued
   attachments and cancel waits. Drain already issued I/O using the storage
   deadline/quarantine rules, then close descriptions and release locks/maps.
   Service replies arriving after closure are discarded using the session id
   and retained generation. No descriptor or mapping is rebound to the next user.
4. SYSTEM services finish already accepted bounded saves tagged to the old
   session, flush that user's settings, then retire their peer links/hive state.
   A failed save is reported; it does not resurrect user execution or change
   the destination uid. Only after teardown is complete does the session
   become DEAD and the secure logon UI regain exclusive input/display.

No wait for a client or disk may hold the session lock. Closure is idempotent.
An issued write may complete to its original object during draining; there is
no false promise of undoing committed disk bytes. If a device cannot be
fenced, retain its buffers and deny a new interactive session until containment
is established. Error/status is recoverable, never a new panic.
Lock-screen operation does not mint a new credential or session: SYSTEM
owns the secure input/display transition and verifies unlock credentials.
An untrusted desktop overlay alone is insufficient protection.

### Queries and IPC subjects

QUERY_SELF returns the complete public immutable credential. libc supplies
32-bit uid_t/gid_t, getuid=geteuid=uid and getgid=getegid=primary gid.
getgroups(0,NULL) reports the required supplementary count; a negative or
insufficient count fails EINVAL without partial output. The supplementary
list may include the primary gid, consistently, and is sorted. Identity
setters are unsupported/EPERM, including requests from administrator uid 500;
they do not become setuid gateways.

Win32 OpenProcessToken/GetTokenInformation/CheckTokenMembership must query
the kernel credential, not an advapi32 or desktop cache that a process can
edit. TokenUser, TokenGroups and session queries are read-only views; requests
to create/assign/adjust credentials are refused. A query token handle is not
an authority transferable to CreateProcessAsUser. Full Windows SID encoding
and SDK facade coverage require the coordinated reservation in Q2.

QUERY_PROCESS permits an administrator to query other non-SYSTEM processes,
or a process to query its own uid in its own session; SYSTEM can query all.
It returns no password material, environment, command line or file contents.
PID 1 and services disclose only the explicitly public system identity.
Regular process listings remain bounded public diagnostic summaries; private
module paths/registers/socket ownership details are filtered as specified below.

Kernel-created IPC links retain the actual peer process and credential/session.
The service can query the link's peer; it must not re-resolve a stale PID after
an asynchronous wait or trust a uid/session in the request. Channel sender_pid
is already overwritten by the kernel (execution-abi.md:1458–1475); preserve
that guarantee. Link closure or session revocation cancels pending work.
SYSTEM brokers carry an explicit immutable request subject to VFS/registry
checks. This is internal request attribution, not thread impersonation or
a user-settable credential handle.

SYSTEM boot/service images, their manifests/configuration, loader seeds and
search paths MUST be protected from administrator replacement through an
indirect route; otherwise an administrator could run arbitrary code as uid 0.
The smallest sealed list includes kernel/loader boot inputs, logon/account
service and security/registry brokers. General application installation remains
administrator-writable. Updating sealed components is a separate SYSTEM-owned
maintenance transaction, never an arbitrary-path account-service RPC.

## Filesystem path policy

### Identity and evaluation

This is a CiukiOS policy over FAT12/FAT16/FAT32 volumes, not an on-disk FAT
ACL or NTFS security descriptor. The system-volume binding uses the mounted
volume identity, not the text "C:" alone. Every /mnt/drive alias, POSIX path,
DOS drive mapping and Win32 path resolves to the same mount/object policy.
An unrecognised fixed volume is admin-writable/readable data by default for
administrators and read-only public data for ordinary users; it cannot become
an unrestricted second route into the system volume. See Q3 for additional
managed data roots.

Authorize the resolved object/parent and operation under the existing VFS
namespace synchronization. Prefixes consist of canonical path components
resolved through the filesystem's pinned case comparison, with component
boundaries. "/Systematic" is not "/System". LFN and its 8.3 alias resolve to
the same short-entry identity and policy label; never authorize an input
string such as PROGRA~1 independently. "." and "..", relative cwd, redundant
separators and native drive mounts follow the actual resolver's parents.
Win32 trimming of trailing dots/spaces happens before resolution; native
rejection of those names stays unchanged. Invalid UTF-8 or malformed LFN
chains fail lookup; no permissive fallback or locale-dependent folding.

Check traversal of each real directory before following the next component,
including "..". Do not collapse "other-user/../mine" before checking access
to other-user. Imported aliases, non-ASCII case pairs, trailing-dot Win32
paths and native /mnt aliases must agree. No symlink, junction or alternate
data stream is added here. If later introduced, it must enter this same
resolved-object authorization pipeline before being exposed.

The policy is a bounded component trie/sorted prefix table with inherited
labels cached on resolved nodes. Never allocate an ACL per regular file or
walk the entire on-disk profile tree at every open. Policy updates invalidate
labels by generation. Existing references retain mount/media generation
and the original object identity; deletion/reuse of a FAT directory slot
must not retarget a retained reference.

### Proposed rule table

Paths below use the native system-volume spelling. G32's numeric profile
layout is the proposed canonical PROFILE(uid) =
/Windows/Profiles/u%08x; Q1 requires agreement with the other contract part.
"Read" includes lookup/stat/enumeration and loading executable bytes when the
object is executable by existing format rules. "Modify" includes create,
write, delete, rename, truncate, attributes and timestamps, subject to the
separate parent rules. All ordinary accesses still obey FAT read-only bits,
share/range locks, mount state and the device lease rules.

| Prefix / object | Standard Users | Guest | Administrators | SYSTEM / special rule |
| --- | --- | --- | --- | --- |
| System volume root | Read; create a new ordinary data directory, not a root file | Read only | Read/modify ordinary entries | Installed anchors are sealed against rename/delete/replacement; new root data directories receive creator metadata before publication. |
| /Windows, /Windows/System, /Windows/System32, /Windows/Fonts | Read | Read | Read/modify application/shared files | More specific sealed/security/profile rules win. No global writable TEMP exception. |
| /System and /Programs, /Program Files, Common Files, bundled game binaries | Read | Read | Read/modify ordinary program content | Protect the sealed executable/boot-input subset separately. Existing Win32 denial of all C:/System writes remains an additional restriction until its consumers migrate; see Q4. |
| Kernel/boot inputs, SYSTEM service images and security policy manifests | Read executable/nonsecret content only | Same | Read only | SYSTEM maintenance only; path ancestors cannot be renamed or replaced. Includes files consumed as privileged code/configuration during the next boot. |
| /System/Security, account verifiers, owner map, security journal/backup/temp/locks | No access | No access | No direct access | SYSTEM only. Parent enumeration must not disclose file contents; account management uses the authenticated broker. |
| /Windows/Profiles container | Enumerate nonsecret account directory names, traverse only an allowed profile | Same | Read | SYSTEM creates/removes/relocates profile root anchors. A user's name is not their ownership proof. |
| PROFILE(uid), including Documents, Desktop, Favorites, Start Menu, application data, settings and Temp | Owner read/modify; others no access | Only Guest's own profile | Read/modify other profiles' ordinary data | SYSTEM maintains root ownership. Security subtrees remain SYSTEM-only even to a profile owner. |
| Default profile template | Read only | Read only | Read/modify template data | SYSTEM creates each private profile by copy under its new owner. The template never creates an anonymous session. |
| All Users/common Start Menu and desktop | Read | Read | Read/modify | Shared executable launch configuration cannot be changed by ordinary users. |
| /Documents (legacy default/shared documents) | Read/modify shared data | Read only | Read/modify | Explicit shared-data choice; never the automatic private Documents path for uid 1000. No SYSTEM service executes code/configuration from here. Migration ownership is Q1. |
| /system/settings and /System/Registry machine settings | Read nonsecret values; no direct modification | Same | Read/modify ordinary machine settings | Security records excluded; Hardware is read-only inventory; broker-owned hive/journal files excluded. Private Desktop/Users/Programs preferences move under PROFILE(uid). |
| Registry broker hive backing files, transaction slots and journals | No direct access | No direct access | No direct access | SYSTEM registry owner only; logical key requests checked as the client. This includes old SYSTEM.CRG/USER.CRG and their alternate slots until migrated. |
| Per-profile registry files managed directly by native libreg | Owner read/modify | Owner only | Read/modify | May contain only that user's preferences, never account roles, machine policy or another user's data. |
| Pagefile, disk/security logs and retained crash-register reports | No direct read/write | No direct read/write | Read nonsecret diagnostics via API; no raw backing access | SYSTEM-owned; stale paging bytes must not be readable through raw disk or file opens. Per-user crash reports may instead be placed in the owning profile. |
| Unknown fixed-volume ordinary data | Read | Read | Read/modify | SYSTEM can provision an explicit managed/shared/owner root; no implicit metadata trust from FAT labels. |
| Unmanaged removable FAT/USB/floppy | Local active session read/modify ordinary files | Same local-session rule | Same | Shared media has no cross-user confidentiality promise; no trust in foreign owner maps, autorun or service registration. |
| CD/ISO, read-only NTFS and write-protected media | Read | Read | Read | Mutations remain EROFS for every identity. |
| VM private logical disk/COW and saved game overlay | VM owner's session only | Own VM only | Own VM; other users' saved data only through explicit administrative file management | Kernel provider mediates sectors. Persistent saves are under the owning profile, not a machine-global writable COW. |

The root directory creation exception requires a SYSTEM-only side record
mapping each root-created data directory to its creator. Until that record
can be committed, ordinary users cannot create the directory. The rest of
that directory inherits owner-private access; there is no arbitrary public
write permission inherited from the root. This is a bounded CiukiOS
approximation of XP Creator Owner, not general NTFS ACL inheritance.
If the lead chooses a simpler read-only root, it must be an explicit
departure recorded by Q3, not silently called the XP default.

### Owner metadata and consistency

Store profile/root-owner metadata under the sealed SYSTEM security directory.
Each record contains schema version, installation/managed-volume identifier,
canonical root locator, uid, policy kind, generation and integrity checksum.
Names displayed at logon, directory spelling, FAT owner-less attributes and
recyclable exported file indices are never the sole owner identity.
Register root anchors from resolved entries at mount/provision time and
check their generation. Account deletion leaves the old uid's tree owned
by that retired uid. A new account or recreated directory with the same
name does not inherit those files or their policy record.

Only SYSTEM provisioning may create/remove/rebind an ownership root.
User rename/delete of an ownership anchor, a sealed prefix, its ancestor
or a managed-volume alias is refused. Renaming content inside an ownership
root is permitted if both sides authorize it. Profile renames update the
account display name, not the numeric storage root.

Use bounded records, checksums, generation and the registry contract's
intent/new/backup durability protocol; checksum is corruption detection,
not authentication against physical tampering. Provision in a deny-first
state, fsync metadata and directories/volume, then publish the account/root.
On uncertain recovery, keep the affected root inaccessible and retain the
old committed metadata. A stale/missing owner map must never turn a profile
into publicly writable data. Exhausted bounds return ENOSPC/ENOMEM. Mounting
or logon can enter authenticated recovery; no new panic or default profile.
The account-service lock is a kernel-owned mutex/endpoint lifetime lock;
death releases ownership and wakes waiters. Persistent intent is recoverable
transaction state, never an immortal exclusive .lock file.

### Enforcement points and retained handles

| Operation | Required checks before first side effect |
| --- | --- |
| Open existing | Traverse parents; authorize READ and/or WRITE against the resolved object, not just open flags. O_TRUNC additionally requires truncate permission before clearing data. |
| Create / mkdir | Traverse and authorize ADD_FILE/ADD_DIR on the actual parent; select the child's inherited policy and reserve all owner metadata before publication. Reject an attempt to create a reserved/sealed root name, including aliases. |
| Read / pread / directory enumeration / stat | Check the retained subject, live session, object label and rights; hide private directory contents. A direct path query of inaccessible data returns EACCES rather than authorizing existence probes. Public account-name enumeration uses the account service. |
| Write / pwrite / append / truncate | Require an existing writable description and current subject permission. Recheck at admission/commit after waits or namespace/policy-generation changes. A read-only fd never becomes writable because its holder is an administrator. |
| Unlink / rmdir | Authorize removal on the parent and the child; preserve anchor, open-directory, sharing and busy checks. Being allowed to add a file cannot authorize removal of a sealed child. |
| Rename / replacement | Check source object+parent, destination parent and any replaced object; reserve both namespace sides under the normal ordering. Check the destination policy, not just write permission on the source. No path visibility changes before the complete check. |
| Move between owner/policy roots | Ordinary users receive EACCES even if they can write both roots; use copy then delete with independent checks instead. Administrator/SYSTEM may relocate ordinary content only after destination classification and handle invalidation are reserved. Root anchors never move through ordinary rename. Different volumes retain EXDEV. |
| Exec / ELF or PE snapshot / DLL load / DOS EXEC/overlay | Authorize the actual executable/source read before snapshot or cache hit. A shared image cache retains bytes, not an authorization grant. Execute access is checked again when creating a process; shared code pages cannot disclose a denied file through query/debug routes. |
| mmap / section/view | Native anonymous mmap remains unchanged (no file-backed native API). Win32 file mappings require the file's read/write rights; new views and flush/writeback use the retained subject. Writable mappings cannot survive session teardown or a policy-boundary move without revocation. |
| Attribute / timestamp / FAT readonly clear | Require MODIFY_METADATA on the resolved object, including Setup FAT attrib, native FILE_INFO_SET and Win32/DOS equivalents. FAT readonly is an additional check, not administrator authorization. |
| fsync / close / delete-on-close / worker writeback | Sync/close can release an owned resource after revocation. Already committed dirty bytes flush under the original admitted request. A newly deferred delete requires its previously authorized intent and current object identity; no post-logoff path lookup may delete a new user's replacement. |
| Dup / inheritance / section handles / IPC attachment | Rights may only be reduced. Cross-session/credential descriptor transfer is refused except explicit SYSTEM-created safe channels; no privileged-file fd can bypass a path policy or session boundary. |

Resolved labels on live handles are generation checked without another FAT
lookup. A move across a policy boundary marks the affected subtree as moving,
blocks new operations, drains issued requests and invalidates all related
descriptions/views before committing; inability to revoke a live mapping
returns EBUSY without moving. Reopen uses the new destination label.
Neither a retained writable fd nor a cached parent pointer preserves old
rights after that move. Within-root renames retain labels and sharing semantics.
This is intentionally stricter than Windows/POSIX open-handle semantics.
An unlink retains a tombstoned object label until last close, independently
of a later directory-slot reuse.

No policy lock spans filesystem I/O. Acquire a bounded authorization lease
on the immutable subject/session, policy generation and involved objects;
recheck while holding the namespace commit guard. A policy change/closure
prevents new leases and drains old ones. A write admitted before closure
may finish, but no fresh request receives the old user's authority.

### Removable media, DOS and Win32

Removable media is shared local data: Users and an enabled interactive Guest
may read/write files, launch programs as themselves, use CD audio, and request
safe removal after the existing blockers/flush checks. No media grants
administrator status, trusted driver loading or autorun execution as SYSTEM.
Formatting, conversion, defrag, volume-wide scanning and raw floppy snapshots
remain administrator operations. A removable drive holding a managed system
volume retains its protected policy; reclassification as "USB" cannot bypass it.
Surprise removal revokes descriptions by mount/media generation as today.

Each VM retains the launching process's credential/session for its entire
lifetime, including all V86/DPMI workers, file/SFT requests and deferred I/O.
Guest PSP changes select DOS sharing/lock ownership only, never a uid.
All bound native file views, INT21 handle/FCB/LFN/find/rename/attribute calls,
redirector callouts, COM/MZ/overlay loads, spool/printer-file paths and
shell helpers enter the same VFS authorization. Denials become DOS error 5
(and the consistent extended error); no local guest FAT fallback.

INT13, INT25/26, guest block IOCTLs and DPMI I/O may reach only VM-private
logical disks/COW under the admitted provider. A guest drive number,
partition offset or forged DFV2 control structure cannot name host sectors.
This applies to administrator-owned VMs too. Do not solve a DOS request by
handing it /dev/ataN. The historical Windows 9x V86 model remains intact.
Private save/reset/export is per uid/profile; uninstalling machine-wide
bundled programs is administrator-only. New-user logon must not attach the
old user's COW, printer output, clipboard text or persistent locks.

Win32 file calls primarily reach kernel w32_file_call directly
(src/kernel/proc/win32/syscall.c:56); they do not all pass through w32srv.
Enforce their VFS requests as the Win32 process itself. w32srv operations
that write INI, hives, shortcuts, installer queues or compatibility settings
must retain the authenticated IPC subject and use the same checked VFS/broker
path. HKLM and machine class/startup entries require administrator; HKCU
binds to the caller's uid/session; HKU permits only one's own data unless
administrator/SYSTEM; .DEFAULT is the protected logon/template hive, never
an anonymous writable HKCU. HKCC/Hardware inventory is read-only to users.
Reads/writes/deletes/enumeration/import/export and key-handle reuse must all
apply the same key rights; opening KEY_READ cannot later authorize SET_VALUE.

The proposed minimal split keeps per-session w32srv under the user's
credential for USER/clipboard/window work and uses a narrow SYSTEM registry
broker for machine hives. The broker authorizes logical HKLM/HKU operations
as the retained client and owns the raw backing files exclusively. Native
libreg private roots move to the profile; machine roots remain protected.
Q5 requires agreement on this service split before implementation. Running
the entire desktop/Win32 shell as SYSTEM to make old files writable is forbidden.

## Devices, administration and authority migration

No identity gains direct port I/O, MMIO, physical memory mapping, arbitrary
DMA, kernel priority or a new panic-triggering diagnostic interface.
Administrator permission enables a supported management request; existing
lease, quiescence, firmware and recovery checks still decide whether it is safe.

| Existing authority / operation | New authorization and lifecycle |
| --- | --- |
| supervisor/PID 1, internal boot/file-service tasks | SYSTEM/session 0, explicitly assigned at boot. Kernel controller ownership never derives from pid/ppid supplied by a caller. |
| dosvm_capability | Own-session VM creation and management; no path-derived user privilege. Preserve one VM per admitted owner and the current VM generation checks. |
| programs_capability | Administrator for machine game uninstall; own-session for own saved-game query/reset/export. Administrative changes to another user's saves use explicit file management, not an ambiguous current-user SAVE command. |
| network_capability and dhcp_capability | Administrator for SET/RENEW/RELEASE and driver/network configuration, including Terminal DHCP commands. Public status remains anyone. Trusted boot DHCP work runs as SYSTEM. |
| time_capability; CIUKI_TIME_AUTHORITY | Administrator for SET_ZONE/SET_LOCAL/SET_UTC/ACKNOWLEDGE. The AUTHORITY output bit is computed from credential, never accepted as input. GET's automatic DST maintenance is internal SYSTEM clock work, not a user-selected set operation. |
| recovery_capability; CIUKI_RECOVERY_AUTHORITY | Administrator for SET/ENABLE/SCANDISK including read-only scans of a whole protected volume; anyone for bounded status/report. Safe/command-only boots still authenticate; they do not grant terminal uid 0. |
| setup_capability | SYSTEM Setup service, restricted to Setup boot and the one supervisor-created process; noninherited, cleared on exec, revoked on exit. A user-run installer is an administrator application, not OS Setup. |
| CIUKI_TASKS_FD, --authority-fd, task authority object | Compatibility handles may remain parseable during migration but confer zero additional rights. Own uid+session may terminate own nonprotected processes; administrator may terminate other users' nonprotected processes; SYSTEM controls protected services. Validate fd kind where supplied, then authorize from credentials. |
| CIUKI_MIXER_FD, mixer_control, public /dev/mixer | Session desktop may adjust current master/line controls; this is a per-user interactive operation, not a grant to write machine registry files. Persist a user's preference in their profile; device-wide boot defaults require administrator/SYSTEM. Strip the control handle at logoff. |
| Display/input grants, channel FOCUS/UNFOCUS/INPUT hints | Own-session designated compositor/input owner; SYSTEM supplies and revokes the exclusive grants. Surface fd possession never gives input snooping. Secure logon/lock uses SYSTEM-owned display/input while user grants are revoked. |
| w32 service/desktop/native-control launch roles | Own-session endpoint ownership registered by SYSTEM, plus peer/session checks. Native Control Panel access is no longer a privilege for that exact path; administrative RPCs test the requester. A client cannot register a replacement SYSTEM service. |
| Delegated Win32 presentation, accelerated batch/fence, DOS fullscreen | Own-session compositor/VM/presentation holder with exact resource token/generation. No cross-session input, framebuffer, shared object or cursor access. |
| /dev/ataN raw pread | Administrator; SYSTEM for managed security/boot/pagefile sectors. Proposed conservative rule: deny administrator raw reads of any managed system disk, allow non-system disk reads. Q4 settles the SYSTEM-only/administrator boundary without exposing verifiers through sectors. |
| /dev/ataN pwrite, DISK_PARTITION_ACQUIRE | Administrator on non-system disks plus exclusive lease; SYSTEM Setup plus Setup mode for the destination boot disk. Never write mounted/pagefile/log disks. Check the actual caller at each raw I/O even for an inherited fd. |
| DISK_LEASE_ACQUIRE/PUBLISH/RECHECK/SET_PHASE | SYSTEM Setup-only transaction as today; check uid 0 in addition to Setup boot/lease ownership. DISK_LEASE_RELEASE/FLUSH require the actual authorized lease holder. |
| Format/partition/convert/ScanDisk/defrag | Administrator, plus exclusive volume/device admission. Boot repair runs internally as SYSTEM. Read-only public geometry/status queries remain anyone; scans must not expose private filenames or bypass ACL-like policy. |
| Driver installation/loading, hardware profile, BOOT.CFG, startup selection | Administrator requests supported changes; SYSTEM consumes protected boot plans and owns hardware bindings. No dynamic driver-loader syscall exists here; do not invent one. Unimplemented operations keep ENOSYS/EOPNOTSUPP. |
| Shutdown/restart/standby/cancel standby, including FORCE actions | Authenticated active interactive session (Users or enabled Guest), through session owner/normal shutdown protocol; SYSTEM Setup/logon may also act. Direct privileged execution checks session ownership. FORCE retains explicit coordinator confirmation, stopped/DMA proof and bounded existing deadlines. |
| Persistent machine power policy | Administrator; current-session display blanking/DPMS and user preference are own-session. Hardware status/events are anyone, without consuming another session's input. |
| Display mode/refresh/monitor | Local session's compositor may switch supported modes/DPMS/screen power; ordinary clients request it through the compositor. Installing a driver or changing boot hardware profile is administrator/SYSTEM work. Read-only mode/monitor queries may be public. |
| COM/LPT, joystick, keyboard | Session-owned exclusive device stream/configuration within its existing bounds; permanent machine routing/mouse binding/hardware calibration defaults require administrator. Per-user calibration/preferences use the profile. No access to another VM's or SYSTEM logger's reserved port. |
| Recovery account | CiukiAdministrator is an administrator, not SYSTEM. Safe Mode merely admits its password-authenticated recovery session despite normal disabled/hidden state. Account service and offline CD rescue tools retain their separate trusted roles. |

Local interactive power permission follows the workstation model, not a
remote shutdown right. Guest receives it as an explicit CiukiOS choice when
Guest is enabled; R6's Users group alone does not prove Guest membership.
The secure logon UI may expose only shutdown/restart without giving an
unauthenticated process an interactive credential. Sleep/wake, forced
shutdown and the existing query/end-session protocol remain coordinated;
no arbitrary unprivileged program can obtain a raw reset or firmware-write
primitive through a new call.

Protected processes are PID 1, kernel controller owners and designated SYSTEM
security/boot services. An administrator's Task Manager cannot kill/debug/read
their memory or duplicate privileged handles. Ordinary same-session ownership
may broaden the old descendant-only rule for call 71; call 54 retains PGID
semantics plus the uid/session check. Cross-user termination requires
administrator and a nonprotected target. Same-session peers do not acquire
each other's address spaces; Win32 READ_MEMORY/WRITE_MEMORY/CONTEXT remain
self/same-process as currently supported. No new cross-process debugger is
introduced by this contract.

## Proposed ABI additions (not implemented)

The lead must reserve the syscall/operation numbers jointly with the user
contract (Q2). Names below are proposals, not occupied numbers. Preserve
native ABI version 1 and every existing record prefix. Add a separately
versioned CIUKI_SYS_SECURITY transport with copied version/size/op/reserved
header, fixed u32 addresses, scoped pack(4), six-register convention and
negative errno. No compiler enum, native pointer, uid_t, time_t or host long
may appear in its wire records.

Proposed ciuki_credential, size 64, alignment 4:

| Field (one declaration per line in the eventual header) | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t uid | 8 | 4 |
| uint32_t primary_gid | 12 | 4 |
| uint32_t group_count | 16 | 4 |
| uint32_t groups[8] | 20 | 32 |
| uint32_t session_id | 52 | 4 |
| uint32_t flags | 56 | 4 |
| uint32_t reserved | 60 | 4 |

The internal record is immutable after construction; version/size are
wire-only overhead if the kernel uses an internal shared representation.
Proposed flags describe INTERACTIVE and SAFE_RECOVERY only; no writable
ADMIN/SYSTEM flag. Unused group slots/reserved are zero; groups are sorted,
unique and validated against the accepted account type.

Proposed ciuki_security_args, size 64, alignment 4:

| Field | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t operation | 8 | 4 |
| uint32_t reserved0 | 12 | 4 |
| uint32_t credential_address | 16 | 4 |
| uint32_t spawn_address | 20 | 4 |
| uint32_t session_id | 24 | 4 |
| uint32_t target_pid | 28 | 4 |
| uint32_t profile_address | 32 | 4 |
| uint32_t profile_bytes | 36 | 4 |
| uint32_t output_address | 40 | 4 |
| uint32_t output_bytes | 44 | 4 |
| uint32_t flags | 48 | 4 |
| uint32_t result | 52 | 4 |
| uint32_t reserved1[2] | 56 | 8 |

| Proposed operation | Authority and copied inputs/outputs |
| --- | --- |
| QUERY_SELF | Anyone; output_address/output_bytes exactly one credential; all other input/output fields zero. |
| QUERY_PROCESS | Own-session/same uid or administrator for nonprotected targets; SYSTEM for all. target_pid, output credential. |
| QUERY_PEER | Only the owning endpoint server; target_pid is instead forbidden and flags identifies the reserved endpoint-selector form to be frozen in Q2. Return the kernel-retained peer credential, never request-provided identity. Prefer a dedicated fd field in the final record if both contract parts need it. |
| SESSION_CREATE | SYSTEM account service only; credential_address selects a copied uid/groups template with session_id zero, profile_address/bytes selects a bounded root descriptor. Kernel verifies registered owner metadata, allocates the session id and returns it in result. |
| SPAWN_CREDENTIAL | SYSTEM only; session_id selects the live immutable template, spawn_address names a copied existing spawn record; other credential inputs zero. Kernel sets the provisional child's credential and returns the new pid in result. |
| SESSION_END | SYSTEM only, selected session_id; flags=BEGIN or COLLECT as jointly reserved. BEGIN prevents new work; COLLECT reports bounded pending teardown instead of waiting indefinitely. |
| SESSION_QUERY | Own-session or SYSTEM; session_id, result=ACTIVE/CLOSING/DEAD. Administrator may query another session's nonsecret state but cannot impersonate it. |

QUERY_PEER's exact field shape and the profile descriptor require the joint
freeze; do not implement the placeholder flags encoding above. A native
credential query and read-only Win32 token query must expose the same value.
Do not add new identity fields to the existing 80-byte proc_record: either
use QUERY_PROCESS or a new versioned enumeration. Call 70's old prefix stays
binary compatible. No external Windows SID mapping may be guessed in libc.

Authorization failures are EPERM for privileged operations, EACCES for
object/path access, EBADF for invalid fd, ESRCH for stale permitted process
lookups, EINVAL for malformed/unknown flags, EOPNOTSUPP for unsupported
version, EFAULT for inaccessible spans, EAGAIN for transient generation/
allocation-limit contention, EBUSY for a closing/moving/live mapping barrier,
ENOMEM for allocation failure, ENOSPC for persistent bounded storage.
Unauthorized target queries must not return private data before failing.
Native existing fd I/O rows need their error sets extended to permit EACCES
on revoked rights; do not translate a security denial into "read-only media".
Win32 maps object denials to ERROR_ACCESS_DENIED (5), privilege denials to
ERROR_PRIVILEGE_NOT_HELD (1314) when that API requires it, and transport
denials to STATUS_ACCESS_DENIED. DOS uses access denied 5.

All new records and every public constant, including uid/group/flag/op/
session-state numbers, need independent literal oracle expectations in
tests/host/abi_layout_test.c. Record size, alignment, array length and every
field offset must be covered; abi.h and every public SDK header declare one
struct field per line so the target layout extractor sees them. Synchronize
kernel/SDK copies and the pointer-free i686 dump. Run the ABI section of
scripts/test/host_kernel_tests.sh serially under nice -n 19 using the
repository's existing sanitizer settings, then python3 scripts/gen_api_reference.py.
No ABI files changed in this design task, so these commands are not run here.
No code was vendored; future vendoring that includes a .c file also requires
an nm duplicate-external-symbol check against every linked library.

## Bounded cost, errors and authentication storage

Proposed initial limits are at most 32 static policy prefixes, 32 registered
profile roots, 32 creator-owned root data directories, eight groups and four
session records including closing/provisional records. These are reviewable
metadata bounds, not a change to process/VM memory caps. At 64 processes,
one inline 64-byte public-shaped credential would cost at most 4096 bytes;
shared internal records can reduce that. Actual node/handle-label and trie
costs must be measured in sec-k02/sec-k03 rather than claimed free.

Permission checks on read/write/fault/writeback are O(1): subject/session
state, granted operation mask, policy generation, mount/media generation
and node label. Prefix selection is bounded by path components and at most
32 static rules at initial resolution. Ownership is inherited from registered
root anchors (at most 64 dynamic owner records), without scanning the owner
database on each open. Permission checks perform no PBKDF2, registry parsing,
disk I/O or whole-profile scan. Existing lookups already read directory
entries; resolving an alias adds no independent security-name scan.
No disk read is added to a warm fd read/write. At most eight group
comparisons are needed at credential creation; an internal precomputed
administrator bit may be cached only as a derived kernel value.

The owner map is loaded/validated once at protected mount/provision and
updated transactionally. No authentication or settings locks on scheduler,
IRQ, sector-transfer or per-pixel paths. Registry/service operations carry
retained subjects, with no account database access per file operation.
Namespace locks protect identity/commit; service locks must be released
before calling into blocking storage. Use timed recoverable synchronization
for new locks so existing lock-cycle assertions do not become new user-
triggerable panic paths.

Account verifiers remain PBKDF2-HMAC-SHA-256, 16-byte fresh getentropy salt
and 32-byte verifier with constant-time comparison and secret erasure.
Keep G32's 10,000 iterations as the initial versioned work factor. The
account format must record algorithm/iterations per verifier, validate a
bounded iteration range before work and support future rehash on successful
authentication. Entropy-not-ready returns a retryable failure; it never
substitutes a tick counter or weak salt. Only the SYSTEM account service
reads database/verifiers, including their temporary/backup/recovery files.
Never send salt/verifier/password in logs, process arguments, environment,
registry exports, crash-report records or a token query.

PBKDF2 runs on one bounded service worker with a bounded request queue.
Cancelled/unknown-user/disabled-account attempts do not bypass scheduling
bounds or leak verifier records. Rate-limit attempts without holding global
locks during derivation. The proposed default account lockout threshold is
zero: failed guesses never create an indefinite lockout or disable the
recovery account. A delayed retry/backoff is not a persisted account lock.
Exact delay/queue/user-limit constants and ready-image recovery-password
provisioning remain joint lead decisions (Q1/Q6/Q7).

Target acceptance should measure derivation on the PIII baseline with the
production SHA-256 implementation and record median/max latency and UI
responsiveness under load. Propose a 250–1000 ms authentication-work target,
not a measured result. Do not tune down to that range without lead review;
do not perform KDFs while holding the VFS, session or input/display locks.

| Failure | Required containment |
| --- | --- |
| Unknown/missing credential, malformed service request | Deny before side effects; cleanup temporary allocations; no fallback to SYSTEM or CiukiUser. |
| Account database corrupt/missing after provisioning | Refuse normal/automatic logon and offer authenticated recovery when usable records exist; no auto-recreation of empty-password accounts. |
| Interrupted account/profile transaction | Recover old committed state or complete the authenticated transaction; no account visible with an unprotected root. |
| Account service crash/stale lock | Revoke its endpoint, drop its kernel lock ownership, cancel pending authentication, recover journal, restart through supervisor; untrusted clients cannot claim its role. |
| Session/uid/policy-generation exhaustion | Return bounded error; never wrap/reuse. |
| OOM / full disk / write protection / broken mount | Preserve old policy; refuse publication/mutation; return normal error and keep recovery evidence. |
| Policy change/rename versus open/write/map | Revalidate or refuse EBUSY/EACCES; no previously approved input string reused as authorization. |
| Logoff versus worker/VM/APC/service reply | Subject remains retained until safe drain, then revoked; next session receives no stale data or authority. |
| Corrupt FAT/aliases/foreign owner map | Deny managed-root access or quarantine/read-only per storage contract; do not turn malformed policy into public data. |
| Denial-log flooding | Bounded counters/rate-limited fixed diagnostic records; no paths/passwords/private buffer content in public logs; logging failure does not panic. |

## Complete syscall and action inventory

The tables below inventory callable operations, including device commands
and multiplexed subactions. Geometry, errno, pixel formats, status bits,
event codes and record fields are parameters/results, not additional calls.
They inherit the enclosing row's authorization and cannot be used to bypass
it. Unknown/reserved operations retain their existing rejection.

The native table gives one row for each published CIUKI_SYS_* symbol in
abi.h, including reserved 6–15. The evidence column names the current
entry/implementation; the declaration citation is the exact baseline line.
Current "process/own fd" means there is no uid/session check today; it is
not a statement that credentials already exist.

| Number / symbol | Current authority | Proposed class / condition | Current evidence |
| --- | --- | --- | --- |
| 0 CIUKI_SYS_EXIT | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:669; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 1 CIUKI_SYS_YIELD | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:670; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 2 CIUKI_SYS_DEBUG_WRITE | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:671; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 3 CIUKI_SYS_PROBE_REPORT | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:672; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 4 CIUKI_SYS_SLEEP_MS | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:673; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 5 CIUKI_SYS_PROBE_QUERY | F0 caller; bounded buffers/arguments; no identity role. | anyone; own execution/diagnostic data only | src/kernel/include/ciuki/abi.h:674; docs/design/execution-abi.md:194; src/kernel/core/syscall.c:125 |
| 6 CIUKI_SYS_RESERVED_6 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:675; docs/design/execution-abi.md:187 |
| 7 CIUKI_SYS_RESERVED_7 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:676; docs/design/execution-abi.md:187 |
| 8 CIUKI_SYS_RESERVED_8 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:677; docs/design/execution-abi.md:187 |
| 9 CIUKI_SYS_RESERVED_9 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:678; docs/design/execution-abi.md:187 |
| 10 CIUKI_SYS_RESERVED_10 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:679; docs/design/execution-abi.md:187 |
| 11 CIUKI_SYS_RESERVED_11 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:680; docs/design/execution-abi.md:187 |
| 12 CIUKI_SYS_RESERVED_12 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:681; docs/design/execution-abi.md:187 |
| 13 CIUKI_SYS_RESERVED_13 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:682; docs/design/execution-abi.md:187 |
| 14 CIUKI_SYS_RESERVED_14 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:683; docs/design/execution-abi.md:187 |
| 15 CIUKI_SYS_RESERVED_15 | Reserved, ENOSYS. | No class/grant: remain ENOSYS | src/kernel/include/ciuki/abi.h:684; docs/design/execution-abi.md:187 |
| 16 CIUKI_SYS_SPAWN | Parent process; ordinary fd inheritance; path/parent delegation can add roles. | own-session; child inherits credential; alternate credential SYSTEM only | src/kernel/include/ciuki/abi.h:685; src/kernel/proc/spawn.c:329; src/kernel/proc/spawn.c:363 |
| 17 CIUKI_SYS_WAITPID | Direct child wait/reap. | own-session; existing child relation still required | src/kernel/include/ciuki/abi.h:686; docs/design/execution-abi.md:1108 |
| 18 CIUKI_SYS_GETPID | Caller process identifiers. | anyone; self | src/kernel/include/ciuki/abi.h:687; src/kernel/proc/syscalls_proc.c:301 |
| 19 CIUKI_SYS_GETPPID | Caller process identifiers. | anyone; self | src/kernel/include/ciuki/abi.h:688; src/kernel/proc/syscalls_proc.c:301 |
| 20 CIUKI_SYS_THREAD_CREATE | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:689; src/kernel/proc/syscalls_proc.c:286 |
| 21 CIUKI_SYS_THREAD_EXIT | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:690; src/kernel/proc/syscalls_proc.c:286 |
| 22 CIUKI_SYS_TLS_SET | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:691; src/kernel/proc/syscalls_proc.c:286 |
| 23 CIUKI_SYS_WAIT_WORD | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:692; src/kernel/proc/syscalls_proc.c:286 |
| 24 CIUKI_SYS_WAKE_WORD | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:693; src/kernel/proc/syscalls_proc.c:286 |
| 25 CIUKI_SYS_MMAP | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:694; src/kernel/proc/syscalls_proc.c:286 |
| 26 CIUKI_SYS_MUNMAP | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:695; src/kernel/proc/syscalls_proc.c:286 |
| 27 CIUKI_SYS_MPROTECT | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:696; src/kernel/proc/syscalls_proc.c:286 |
| 28 CIUKI_SYS_BRK | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:697; src/kernel/proc/syscalls_proc.c:286 |
| 29 CIUKI_SYS_OPEN | Path/flags/share/mount checks; no uid policy. | own-session plus path policy; administrator/SYSTEM where prefix requires | src/kernel/include/ciuki/abi.h:698; src/kernel/proc/syscalls_file.c:332; src/kernel/proc/fdtable.c:197 |
| 30 CIUKI_SYS_READ | Owned fd/access mode; Win32 System restriction; raw sectors separately routed. | own-session and retained object policy; raw/admin/SYSTEM exceptions below | src/kernel/include/ciuki/abi.h:699; src/kernel/proc/syscalls_file.c:141; src/kernel/proc/fdtable.c:314 |
| 31 CIUKI_SYS_WRITE | Owned fd/access mode; Win32 System restriction; raw sectors separately routed. | own-session and retained object policy; raw/admin/SYSTEM exceptions below | src/kernel/include/ciuki/abi.h:700; src/kernel/proc/syscalls_file.c:141; src/kernel/proc/fdtable.c:314 |
| 32 CIUKI_SYS_PREAD | Owned fd/access mode; Win32 System restriction; raw sectors separately routed. | own-session and retained object policy; raw/admin/SYSTEM exceptions below | src/kernel/include/ciuki/abi.h:701; src/kernel/proc/syscalls_file.c:141; src/kernel/proc/fdtable.c:314 |
| 33 CIUKI_SYS_PWRITE | Owned fd/access mode; Win32 System restriction; raw sectors separately routed. | own-session and retained object policy; raw/admin/SYSTEM exceptions below | src/kernel/include/ciuki/abi.h:702; src/kernel/proc/syscalls_file.c:141; src/kernel/proc/fdtable.c:314 |
| 34 CIUKI_SYS_LSEEK64 | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:703; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 35 CIUKI_SYS_CLOSE | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:704; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 36 CIUKI_SYS_FSTAT | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:705; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 37 CIUKI_SYS_STAT | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:706; src/kernel/proc/syscalls_file.c:323 |
| 38 CIUKI_SYS_GETDENTS | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:707; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 39 CIUKI_SYS_MKDIR | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:708; src/kernel/proc/syscalls_file.c:323 |
| 40 CIUKI_SYS_RMDIR | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:709; src/kernel/proc/syscalls_file.c:323 |
| 41 CIUKI_SYS_RENAME | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:710; src/kernel/proc/syscalls_file.c:323 |
| 42 CIUKI_SYS_UNLINK | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:711; src/kernel/proc/syscalls_file.c:323 |
| 43 CIUKI_SYS_DUP | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:712; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 44 CIUKI_SYS_DUP2 | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:713; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 45 CIUKI_SYS_FCNTL | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:714; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 46 CIUKI_SYS_FSYNC | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:715; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 47 CIUKI_SYS_FTRUNCATE | Owned fd/kind/access flags/share/volume state; no uid policy. | own-session and retained policy; close/drain cleanup allowed after revocation | src/kernel/include/ciuki/abi.h:716; src/kernel/proc/syscalls_file.c:235; src/kernel/proc/syscalls_file.c:429 |
| 48 CIUKI_SYS_GETCWD | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:717; src/kernel/proc/syscalls_file.c:323 |
| 49 CIUKI_SYS_CHDIR | Caller cwd/path namespace and FAT/share checks; no uid policy. | own-session plus object/parent path policy; administrator/SYSTEM for protected mutations | src/kernel/include/ciuki/abi.h:718; src/kernel/proc/syscalls_file.c:323 |
| 50 CIUKI_SYS_CLOCK_GETTIME | Clock query / calling thread sleep. | anyone; CPU clock and sleep are self only | src/kernel/include/ciuki/abi.h:719; src/kernel/proc/syscalls_file.c:432 |
| 51 CIUKI_SYS_NANOSLEEP | Clock query / calling thread sleep. | anyone; CPU clock and sleep are self only | src/kernel/include/ciuki/abi.h:720; src/kernel/proc/syscalls_file.c:432 |
| 52 CIUKI_SYS_SIGACTION | Own signal dispositions/mask/frame or own-process TID. | own-session; existing narrower self/thread checks remain | src/kernel/include/ciuki/abi.h:721; docs/design/execution-abi.md:983; docs/design/execution-abi.md:1276 |
| 53 CIUKI_SYS_SIGPROCMASK | Own signal dispositions/mask/frame or own-process TID. | own-session; existing narrower self/thread checks remain | src/kernel/include/ciuki/abi.h:722; docs/design/execution-abi.md:983; docs/design/execution-abi.md:1276 |
| 54 CIUKI_SYS_KILL | Caller PGID only, including signal 0. | own-session and same uid plus existing PGID rule; no SYSTEM targets | src/kernel/include/ciuki/abi.h:723; docs/design/execution-abi.md:979; docs/design/execution-abi.md:1279 |
| 55 CIUKI_SYS_SIGRETURN | Own signal dispositions/mask/frame or own-process TID. | own-session; existing narrower self/thread checks remain | src/kernel/include/ciuki/abi.h:724; docs/design/execution-abi.md:983; docs/design/execution-abi.md:1276 |
| 56 CIUKI_SYS_UNAME | Public copied system information. | anyone; no secret account data | src/kernel/include/ciuki/abi.h:725; src/kernel/proc/syscalls_file.c:38 |
| 57 CIUKI_SYS_SURFACE_CREATE | Owned surface; transferred surfaces read-only. | own-session; retained rights, zero initialization and session namespace | src/kernel/include/ciuki/abi.h:726; src/kernel/proc/syscalls_desktop.c:69; docs/design/execution-abi.md:1402 |
| 58 CIUKI_SYS_SURFACE_MAP | Owned surface; transferred surfaces read-only. | own-session; retained rights, zero initialization and session namespace | src/kernel/include/ciuki/abi.h:727; src/kernel/proc/syscalls_desktop.c:69; docs/design/execution-abi.md:1402 |
| 59 CIUKI_SYS_SURFACE_INFO | Owned surface; transferred surfaces read-only. | own-session; retained rights, zero initialization and session namespace | src/kernel/include/ciuki/abi.h:728; src/kernel/proc/syscalls_desktop.c:69; docs/design/execution-abi.md:1402 |
| 60 CIUKI_SYS_PRESENT | Exclusive owner-bound display/input fd. | own-session compositor/grant owner; SYSTEM secure UI; no admin bypass of ownership | src/kernel/include/ciuki/abi.h:729; src/kernel/proc/syscalls_desktop.c:80; docs/design/execution-abi.md:1393 |
| 61 CIUKI_SYS_INPUT_READ | Exclusive owner-bound display/input fd. | own-session compositor/grant owner; SYSTEM secure UI; no admin bypass of ownership | src/kernel/include/ciuki/abi.h:730; src/kernel/proc/syscalls_desktop.c:80; docs/design/execution-abi.md:1393 |
| 62 CIUKI_SYS_CHANNEL_PAIR | Owned endpoints; copied surface attachments; hints require input grant. | own-session; cross-session links only explicit SYSTEM broker endpoints | src/kernel/include/ciuki/abi.h:731; src/kernel/proc/syscalls_desktop.c:96; docs/design/execution-abi.md:554 |
| 63 CIUKI_SYS_CHANNEL_SEND | Owned endpoints; copied surface attachments; hints require input grant. | own-session; cross-session links only explicit SYSTEM broker endpoints | src/kernel/include/ciuki/abi.h:732; src/kernel/proc/syscalls_desktop.c:96; docs/design/execution-abi.md:554 |
| 64 CIUKI_SYS_CHANNEL_RECV | Owned endpoints; copied surface attachments; hints require input grant. | own-session; cross-session links only explicit SYSTEM broker endpoints | src/kernel/include/ciuki/abi.h:733; src/kernel/proc/syscalls_desktop.c:96; docs/design/execution-abi.md:554 |
| 65 CIUKI_SYS_THREAD_JOIN | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:734; src/kernel/proc/syscalls_proc.c:286 |
| 66 CIUKI_SYS_THREAD_DETACH | Own process/thread/address space; native personality restrictions. | own-session; self, no credential changes | src/kernel/include/ciuki/abi.h:735; src/kernel/proc/syscalls_proc.c:286 |
| 67 CIUKI_SYS_DISPLAY_INFO | Exclusive owner-bound display/input fd. | own-session compositor/grant owner; SYSTEM secure UI; no admin bypass of ownership | src/kernel/include/ciuki/abi.h:736; src/kernel/proc/syscalls_desktop.c:80; docs/design/execution-abi.md:1393 |
| 68 CIUKI_SYS_THREAD_KILL | Own signal dispositions/mask/frame or own-process TID. | own-session; existing narrower self/thread checks remain | src/kernel/include/ciuki/abi.h:737; docs/design/execution-abi.md:983; docs/design/execution-abi.md:1276 |
| 69 CIUKI_SYS_DEVCTL | Per-device fd/kind/command checks; several operations lack role tests. | Split by EVERY command in device table below | src/kernel/include/ciuki/abi.h:738; src/kernel/proc/syscalls_file.c:369 |
| 70 CIUKI_SYS_PROC_LIST | Anyone may enumerate all bounded process summaries. | anyone for existing nonsecret summary; private detail own-session/admin/SYSTEM | src/kernel/include/ciuki/abi.h:739; src/kernel/proc/syscalls_proc.c:212; docs/design/execution-abi.md:1696 |
| 71 CIUKI_SYS_PROC_TERMINATE | Descendant or delegated task-authority fd; protected desktop/owner groups. | own-session same uid; administrator across users; SYSTEM protected-service control | src/kernel/include/ciuki/abi.h:740; src/kernel/proc/syscalls_proc.c:242; docs/design/execution-abi.md:1711 |
| 72 CIUKI_SYS_DOSVM | Path-derived DOSVM/program-management flags or public query. | Split by all 23 DOSVM operations below | src/kernel/include/ciuki/abi.h:741; src/kernel/vm/dosvm_client.c:300; docs/design/execution-abi.md:1739 |
| 73 CIUKI_SYS_POWER | Live desktop/Setup for actions; STATUS/EVENTS public in implementation. | anyone status; own-session interactive power; administrator machine policy; SYSTEM Setup/logon | src/kernel/include/ciuki/abi.h:742; src/kernel/core/power.c:515; src/kernel/core/power.c:535 |
| 74 CIUKI_SYS_DISPLAY | QUERY public; display mutations restricted to desktop/grant. | anyone query; own-session compositor mutation; SYSTEM secure UI | src/kernel/include/ciuki/abi.h:743; docs/design/execution-abi.md:2053; src/kernel/include/ciuki/abi.h:2315 |
| 75 CIUKI_SYS_LAUNCH_MARK | STATUS public; BEGIN/CONFIGURE/DAMAGE/FRAME live desktop only. | anyone status; own-session compositor for its child launch marks | src/kernel/include/ciuki/abi.h:744; src/kernel/proc/syscalls_proc.c:88; docs/design/execution-abi.md:2067 |
| 76 CIUKI_SYS_DEVICES | Public read-only inventory. | anyone; no activation or security-state mutation | src/kernel/include/ciuki/abi.h:745; docs/design/execution-abi.md:2116; src/kernel/core/syscall.c:175 |
| 77 CIUKI_SYS_DISPLAY_ACCEL | Owned writable display grant; copied accelerator commands. | own-session compositor, all subcommands below; SYSTEM secure UI | src/kernel/include/ciuki/abi.h:746; src/kernel/proc/syscalls_desktop.c:13; src/kernel/include/ciuki/abi.h:2447 |
| 78 CIUKI_SYS_SOCKET | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:747; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 79 CIUKI_SYS_BIND | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:748; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 80 CIUKI_SYS_CONNECT | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:749; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 81 CIUKI_SYS_LISTEN | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:750; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 82 CIUKI_SYS_ACCEPT | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:751; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 83 CIUKI_SYS_SENDTO | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:752; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 84 CIUKI_SYS_RECVFROM | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:753; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 85 CIUKI_SYS_SHUTDOWN | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:754; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 86 CIUKI_SYS_SOCKNAME | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:755; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 87 CIUKI_SYS_SOCKOPT | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:756; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 88 CIUKI_SYS_POLL | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:757; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 89 CIUKI_SYS_RESOLVE4 | Owned socket/fd; bounded address, poll and resolver work; no user-role check. | own-session for sockets/resolver; no privilege by low port number; supported families unchanged | src/kernel/include/ciuki/abi.h:758; src/kernel/net/lwip/syscalls.c:104; docs/design/execution-abi.md:2676 |
| 90 CIUKI_SYS_NETCTL | network_capability; dhcp_capability only for RENEW/RELEASE, even GET otherwise gated. | anyone GET; administrator SET/RENEW/RELEASE; SYSTEM boot client | src/kernel/include/ciuki/abi.h:759; src/kernel/net/lwip/syscalls.c:227 |
| 91 CIUKI_SYS_GETENTROPY | Public bounded entropy; EAGAIN if unseeded. | anyone; entropy readiness unchanged | src/kernel/include/ciuki/abi.h:760; docs/design/execution-abi.md:2708; src/kernel/core/syscall.c:158 |
| 96 CIUKI_SYS_W32 | Win32 personality plus individual native service exceptions. | Split by all Win32 families/actions below | src/kernel/include/ciuki/abi.h:769; src/kernel/proc/win32/syscall.c:30; docs/design/execution-abi.md:2167 |
| 97 CIUKI_SYS_TIME | GET public; time_capability for SET_ZONE/LOCAL/UTC/ACKNOWLEDGE. | anyone GET; administrator setters/ack; SYSTEM automatic DST maintenance | src/kernel/include/ciuki/abi.h:762; src/kernel/proc/timezone.c:288; docs/design/execution-abi.md:2815 |
| 98 CIUKI_SYS_FILE_INFO | GET/SET use path/FAT metadata checks; no uid policy. | own-session; GET read policy, SET modify-metadata policy | src/kernel/include/ciuki/abi.h:763; src/kernel/proc/syscalls_file.c:302; src/kernel/include/ciuki/abi.h:2050 |
| 99 CIUKI_SYS_RECOVERY | STATUS/REPORT public; recovery_capability for SET/ENABLE/SCANDISK. | anyone status/report; administrator configuration/scan; SYSTEM boot recovery | src/kernel/include/ciuki/abi.h:764; src/kernel/proc/syscalls_recovery.c:20; docs/design/execution-abi.md:2836 |
| 100 CIUKI_SYS_NET_INFO | Public config/stats/routes and all-socket snapshot. | anyone config/stats/routes; own-session own sockets; administrator all non-SYSTEM sockets | src/kernel/include/ciuki/abi.h:765; src/kernel/net/lwip/syscalls.c:109; src/kernel/include/ciuki/abi.h:3789 |

Numbers 92–95 are unassigned; 101 and later are not allocated by this
contract. Do not repurpose them without the joint lead reservation.

### Native multiplexed actions

Every action in a grouped row has the same authority unless the row explicitly
splits query/mutation. Evidence names the header declaration and current
authority implementation/contract. Numeric parameters do not change the class.

| Family and every action | Current authority / evidence | Proposed class |
| --- | --- | --- |
| fcntl: F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL, F_DUPFD_CLOEXEC | Own descriptor; abi.h:388–393; proc/fdtable.c:173 | own-session; never increase retained rights or transfer a SYSTEM-only object. |
| Seek SEEK_SET/SEEK_CUR/SEEK_END; wait WNOHANG; clock REALTIME/MONOTONIC/PROCESS_CPUTIME_ID | Own fd/child/CPU clock or public clock; abi.h:395–415; execution-abi.md:943–994 | same own-session/anyone split as enclosing calls. |
| mmap MAP_PRIVATE/MAP_ANONYMOUS, PROT_NONE/READ/WRITE/EXEC; MAP_SHARED/MAP_FIXED unsupported | Private self memory, W+X restrictions; abi.h:402–410; execution-abi.md:1209–1246 | own-session; file-backed native mapping remains unsupported. |
| Spawn NEW_GROUP, thread DETACHED; signal 0 / supported signals | Own creation, own process/PGID signal checks; abi.h:411–416; execution-abi.md:1079–1138 and :1276 | own-session; PGID/flags never change security session/uid. |
| Channel DONTWAIT and FOCUS/UNFOCUS/INPUT delivery hints | Endpoint, input grant for hints; execution-abi.md:554–565 and :1469 | own-session; hint sender must be the designated input owner in that session. |
| POWER REBOOT(1), OFF(2), STANDBY(5), CANCEL_STANDBY(8), FORCE_REBOOT(9), FORCE_OFF(10) | Desktop/Setup gate; abi.h:1825–1826, :1900–1916; core/power.c:535–560 | own-session interactive owner, or SYSTEM Setup/logon; all existing coordination/device barriers apply. |
| POWER QUERY(3), STATUS(4), EVENTS(7) | QUERY owner-only, cached STATUS/EVENTS public; abi.h:1827, :1836, :1902; core/power.c:515–523 and :543 | anyone; no private peer state. |
| POWER POLICY(6) | Desktop/Setup; abi.h:1901; core/power.c:525–533 | administrator for machine policy; per-user preferences through session UI/profile. |
| DISPLAY QUERY(0), MONITOR(4), REFRESH(5) | QUERY public; additional query routes use display authority; abi.h:2315–2327; execution-abi.md:2053 | anyone for public copied inventory; correct grant required on fd-specific query form. |
| DISPLAY SET(1), SCREEN(2), DPMS(3); SCREEN CONFIG(1)/STATUS(2), PERSIST flag | Desktop owner; abi.h:2317–2320, :2368–2371 | own-session compositor; persistent machine defaults administrator, private preferences owner. |
| LAUNCH STATUS(0), BEGIN(1), CONFIGURE(2), DAMAGE(3), FRAME(4) | Status public, other actions desktop and its child PID; abi.h:772–776; proc/syscalls_proc.c:88–125 | anyone status; own-session compositor/child for markers. |
| NETCTL GET(0), SET(1), RENEW(2), RELEASE(3) | Path-derived network flag, or Terminal DHCP flag for RENEW/RELEASE; abi.h:967–970; net/lwip/syscalls.c:227–250 | anyone GET; administrator SET/RENEW/RELEASE; SYSTEM boot worker. |
| Socket shutdown SHUT_RD/SHUT_WR/SHUT_RDWR; local/peer sockname; get/set sockopt | Own socket description; abi.h:3761–3776; net/lwip/sockets.c:328–386 and :835–841 | own-session; SO_REUSEADDR, SO_BROADCAST, SO_TYPE, SO_ERROR, SO_RCVTIMEO, SO_SNDTIMEO, SO_RX_BYTES preserve supported get/set restrictions. |
| IPv4 resolver synchronous form and START(1)/POLL(2)/CANCEL(3) | Caller-owned resolver request; abi.h:3794–3796; net/lwip/sockets.c:1178–1209 | own-session; another session cannot poll/cancel a retained request. |
| NET_INFO CONFIG(0), STATS(2), ROUTES(3), SOCKETS(1) | Public snapshots; abi.h:3789–3792; net/lwip/syscalls.c:109–133 | anyone config/stats/routes; own-session own socket details; administrator other non-SYSTEM sockets; SYSTEM all. |
| TIME GET(0), SET_ZONE(1), SET_LOCAL(2), SET_UTC(3), ACKNOWLEDGE(4) | Public GET, time flag for others; abi.h:1080–1084; proc/timezone.c:288 | anyone GET; administrator all mutations/acknowledge; automatic DST adjustment attributed internally to SYSTEM. |
| FILE_INFO GET(0), SET(1), attributes/created/accessed/modified masks | Path/FAT checks; abi.h:2050–2067; proc/syscalls_file.c:302–319 | own-session and path read/metadata rights; administrator or SYSTEM where prefix requires. |
| RECOVERY STATUS(0), REPORT(4), SET(1), ENABLE(2), SCANDISK(3), FIX/THOROUGH/FREE_LOST/NO_COPY | Public status/report; flag required for scan/config; abi.h:2100–2120, :2152–2155; proc/syscalls_recovery.c:20–59 | anyone status/report without private findings; administrator all scans and scheduling/repair/config; SYSTEM boot work. |

Here and below, an abbreviated kernel evidence path such as proc/fdtable.c
means src/kernel/proc/fdtable.c; abi.h means
src/kernel/include/ciuki/abi.h.

### Device-control commands (call 69)

Dispatch evidence: proc/syscalls_file.c:369–421. Rights on an open device fd
are still necessary for every command. No command may authorize by fd number
or request bytes alone. Byte read/write paths have the same device/session
checks, including accesses through poll, mmap/sections, dup and inheritance.

| Every command / subaction | Current authority / exact evidence | Proposed class and required condition |
| --- | --- | --- |
| INPUT_HID_GET_CAPS, INPUT_HID_GET_STATE | Input owner; include/ciuki/input_abi.h:6–7, :45; proc/grants.c:349–375 | own-session input owner; clients receive copied input only through that session's compositor. |
| INPUT_HID_CALIBRATE | Input owner, persistent joystick helper; input_abi.h:8; proc/grants.c:376–379 | own-session for current user's calibration; administrator if modifying a machine default. |
| INPUT_JOY_CONTROL: CIUKI_JOY_QUERY, CIUKI_JOY_GET_ROUTE, CIUKI_JOY_NEXT, CIUKI_JOY_SAVE_STATUS, CIUKI_JOY_GET_GAME | Input owner; abi.h:24–35; proc/grants.c:349–379 | own-session input owner, with only that user's route/game preference. |
| INPUT_JOY_CONTROL: CIUKI_JOY_CALIBRATE, CIUKI_JOY_ROUTE, CIUKI_JOY_CONFIGURE, CIUKI_JOY_SAVE_GAME | Input owner, hardware tuple/settings; abi.h:26–31; proc/grants.c:349 | own-session transient/user preference; administrator permanent machine configuration. |
| INPUT_KEYBOARD_SET | Declared input-owner command, handled by proc/grants.c:353; abi.h:2739. The inspected devctl forwarding condition at proc/syscalls_file.c:385 omits this constant. | own-session keyboard policy; audit/fix the existing routing in the later directive, without claiming it works here. |
| COM_GET_LINE, COM_GET_STATUS, COM_GET_OWNER | Correct COM fd/controller generation; abi.h:73, :75, :77; drivers/com.c:578–588 | own-session port owner, or anyone for a separate nonsecret inventory query. |
| COM_SET_LINE, COM_SET_MODEM | Writable COM owner; abi.h:74, :76; drivers/com.c:588 | own-session bounded line/modem control; never another session/VM/SYSTEM logger's port. |
| COM_SET_MOUSE, including OFF/Microsoft/Logitech/Systems/Wheel protocol | Writable owner; abi.h:78–88; drivers/com.c:588–592 | administrator to change machine input binding; SYSTEM serial-mouse provider performs actual binding. |
| LPT_GET_CONFIG, LPT_GET_STATUS | Port fd; abi.h:165, :167; drivers/lpt.c:565–575 | own-session owner/nonsecret inventory. |
| LPT_SET_CONFIG, LPT_INIT | Writable port owner; abi.h:166, :168; drivers/lpt.c:575–578 | own-session stream/init within admitted mode; administrator persistent machine/VM routing. |
| NET_GET_STATE, NET_QUERY_DRIVER_PACK | Public /dev/net query; abi.h:913–914; execution-abi.md:2725–2733 | anyone; pack inspection cannot activate/install drivers. |
| CIUKI_SETUP_FAT_QUERY, CIUKI_SETUP_FAT_ATTRIB | Setup mode plus noninherited Setup flag; abi.h:1028–1029; proc/setup_file.c:24–26 | SYSTEM Setup only; even read metadata retains the Setup transaction boundary. |
| CIUKI_SETUP_CLOCK_SET | Setup mode plus Setup flag; abi.h:1030; proc/setup_clock.c:10–12 | SYSTEM Setup only; no alternate public clock setter. |
| CIUKI_SETUP_NET_QUERY | Declared at abi.h:1031; no special handler in inspected proc/syscalls_file.c:369–421 | Keep unsupported routing unsupported until audited; if implemented, SYSTEM Setup query only. |
| DISPLAY_GET_MONITOR, DISPLAY_GET_REFRESH | Valid display grant; abi.h:2326–2327; proc/syscalls_file.c:370–384 | own-session grant form; public non-fd inventory can use call 74. |
| MEDIA_GET_STATE, MEDIA_GET_VOLUME, MEDIA_GET_SPACE, MEDIA_GET_BLOCKERS | /dev/media; abi.h:1125–1149, :1189; drivers/media_dev.c:137–266 | anyone nonsecret inventory; blocker rows disclose process name/pid only, never private file paths. |
| MEDIA_SAFE_REMOVE, MEDIA_FLOPPY_LOAD | /dev/media, media/generation/blocker checks; abi.h:1126, :1185; drivers/media_dev.c:230 | own-session local media use; refuse busy/protected system media and retain flush/generation checks. |
| MEDIA_GET_LABEL, MEDIA_SET_LABEL | /dev/media, FAT writable-volume checks; abi.h:1209–1210; drivers/media_dev.c:146 | anyone GET; own-session SET on unmanaged removable, administrator SET fixed data volume; SYSTEM sealed/system volume identity. |
| MEDIA_GET_FORMAT, MEDIA_GET_CONVERT | /dev/media queries; abi.h:1229, :1301; drivers/media_dev.c:147–176 | anyone geometry/capability query. |
| MEDIA_FORMAT, QUICK/full/LOW_LEVEL choices; MEDIA_CONVERT | /dev/media plus existing exclusive/busy/fixed restrictions; abi.h:1230–1236, :1302; drivers/media_dev.c:147–176 | administrator, including removable format; SYSTEM privileged boot-volume maintenance. Authorization does not enable unsupported fixed-media formatting. |
| MEDIA_FLOPPY_READ, MEDIA_FLOPPY_WRITE | Exclusive raw diskette snapshot via /dev/media; abi.h:1238–1239; drivers/media_dev.c:143 | administrator, correct medium generation; no host raw disk delegation to DOS. |
| MEDIA_DEFRAG_ANALYZE, MEDIA_DEFRAG_STEP, CHECK/STARTUP options | /dev/media invokes volume engine; abi.h:1272–1275; drivers/media_dev.c:155–161 | administrator; SYSTEM startup maintenance; analyze is volume-wide, not a private-file reading bypass. |
| MEDIA_CD_STATE, MEDIA_CD_INFO, MEDIA_CD_TOC, MEDIA_CD_POSITION | /dev/media, CD identity/generation; abi.h:1329–1331, :1336; drivers/media_dev.c:144 | anyone nonsecret media data. |
| MEDIA_CD_READ_AUDIO | CD capability/extent checks; abi.h:1332; drivers/media_cd.c:152 | own-session audio use; data sectors still go through filesystem policy. |
| MEDIA_CD_TRAY with CIUKI_CD_TRAY_OPEN/CIUKI_CD_TRAY_CLOSE; MEDIA_CD_PLAY, MEDIA_CD_PAUSE with CIUKI_CD_ACTION_PAUSE/CIUKI_CD_ACTION_RESUME/CIUKI_CD_ACTION_STOP | Media fd, serialized CD transport; abi.h:1333–1335, :1360–1366; drivers/media_dev.c:144 | own-session local media owner, no cross-session takeover; retain safe-removal blockers. |
| DISK_GET_INFO, DISK_LEASE_STATE | Public raw node queries; abi.h:1477, :1482; fs/disklease.c:409–413 | anyone bounded geometry/state; no sector contents or security metadata. |
| DISK_LEASE_ACQUIRE | Setup flag and idle disk; abi.h:1478; fs/disklease.c:221–246 | SYSTEM Setup only plus Setup boot and exclusive lease rules. |
| DISK_PARTITION_ACQUIRE | Ordinary process on nonboot idle disk, no uid check; abi.h:1484; fs/disklease.c:224–227 | administrator on non-system disk plus exclusive lease; explicit migration requirement. |
| DISK_LEASE_RELEASE, DISK_FLUSH (also raw fd fsync) | Caller lease authorization; abi.h:1479–1480; fs/disklease.c:76–80 and :258 | administrator authorized nonboot editor or SYSTEM Setup holder; no inherited authorization. |
| DISK_PUBLISH, DISK_SET_PHASE, DISK_RECHECK | Setup flag plus held lease/phase; abi.h:1481, :1483, :1485; fs/disklease.c:404 | SYSTEM Setup transaction only. |
| AUDIO_GET_CAPS, AUDIO_GET_STATUS, AUDIO_GET_PRODUCER | Owned audio stream/caps; abi.h:1596, :1598, :1603; audio/audio_dev.c:997–1020 | own-session stream; public hardware caps may be queried without another user's stream state. |
| AUDIO_SET_FORMAT, AUDIO_SET_GAIN, AUDIO_DRAIN, AUDIO_RESET, AUDIO_REFILL_READ, AUDIO_SET_SOURCE, AUDIO_MIDI_WRITE | Own audio stream/mode; abi.h:1597, :1599–1602, :1643, :1648; audio/audio_dev.c:1023–1142 | own-session; refill hint cannot elevate credential or access another user's source data. |
| AUDIO_CAPTURE_CAPS, AUDIO_CAPTURE_ACQUIRE, AUDIO_CAPTURE_RELEASE, AUDIO_CAPTURE_START, AUDIO_CAPTURE_READ, AUDIO_CAPTURE_STOP | Readable capture fd plus exclusive reservation; abi.h:1604–1607, :1646–1647; audio/audio_dev.c:1020–1081 | own-session active local capture owner; release/revoke at logoff before the next session. |
| MIXER_GET_MASTER, MIXER_GET_DIAGNOSTICS, MIXER_GET_LINE | Public mixer read; abi.h:1638, :1640, :1662; audio/audio_dev.c:1101 and :1163–1171 | anyone nonsecret hardware state. |
| MIXER_SET_MASTER, MIXER_REFRESH_MASTER, MIXER_SET_LINE | Supervisor-installed mixer_control; abi.h:1639, :1641, :1663; audio/audio_dev.c:1102, :1168–1173 | own-session designated mixer owner; administrator/SYSTEM only for persistent machine default. |

### Display/accelerator actions (call 77)

All rows currently require the owned writable display grant, including
queries (proc/syscalls_desktop.c:13–67). All proposed rows are own-session
compositor/grant operations, or SYSTEM for the secure UI, with retained
generation and object rights. Administrator status alone cannot acquire
someone else's scanout, input or mapped surface.

| Every action | Declaration evidence | Required object restriction |
| --- | --- | --- |
| CIUKI_ACCEL_QUERY, CIUKI_ACCEL_COPY, CIUKI_ACCEL_FILL, CIUKI_ACCEL_PATTERN | abi.h:2447–2448 | Current display generation; copies cannot read another session's target. |
| CIUKI_ACCEL_CURSOR_SET, CIUKI_ACCEL_CURSOR_MOVE, CIUKI_ACCEL_CURSOR_SHOW | abi.h:2448–2450 | Session's cursor only. |
| CIUKI_ACCEL_WRITE, CIUKI_ACCEL_OFFSCREEN, CIUKI_ACCEL_QUERY_ENGINE | abi.h:2458–2468 | Copied/pinned source or bounded owner-only offscreen region. |
| CIUKI_ACCEL_BATCH, CIUKI_ACCEL_QUERY_COMMANDS, CIUKI_ACCEL_QUERY_SURFACE | abi.h:2473–2476 | All target/surface ids must belong to the caller's session and resource namespace. |
| CIUKI_ACCEL_FENCE; CIUKI_FENCE_STATUS, CIUKI_FENCE_WAIT, CIUKI_FENCE_CANCEL, CIUKI_FENCE_RELEASE | abi.h:2474, :2504–2507 | Fence ownership/generation retained through wait, cancel, logoff and release. |
| CIUKI_CMD_FILL, CIUKI_CMD_COPY, CIUKI_CMD_PATTERN, CIUKI_CMD_WRITE, CIUKI_CMD_MONO, CIUKI_CMD_INVERT, CIUKI_CMD_KEYED, CIUKI_CMD_SCALED, CIUKI_CMD_LINE, CIUKI_CMD_SYNC | abi.h:2520–2529 | Every embedded target/source/clip rectangle checked before batch effects. |
| CIUKI_CMD_CURSOR_MONO_SET, CIUKI_CMD_CURSOR_ARGB_SET, CIUKI_CMD_CURSOR_MOVE, CIUKI_CMD_CURSOR_SHOW | abi.h:2530–2533 | Same owner restriction as direct cursor commands. |

Surface creation formats XRGB8888 and BATCH_RGB565/BGR888/XRGB8888
(abi.h:2517–2519) are own-session allocations. Batch format selection is not
permission to name scanout or another user's video memory. Existing
unsupported user mapping of managed surfaces remains unsupported.

### DOSVM operations (call 72)

Current ordinary VM operations require dosvm_capability and owner key;
public query and programs_capability exceptions are explicit below.
Declaration evidence is abi.h:2800–2834; dispatcher evidence is
vm/dosvm_client.c:300–638. Proposed VM ownership always includes uid/session
and actual native owner, not a guest PSP or matching uid alone.

| Action | Current authority | Proposed class |
| --- | --- | --- |
| CIUKI_DOSVM_CREATE (1) | DOSVM flag, owned writable surface, admission | own-session; credential fixed before guest boot. |
| CIUKI_DOSVM_PRESENT (2) | Flag + owner key/surface | own-session owner. |
| CIUKI_DOSVM_INPUT (3) | Flag + focused owner | own-session focused input; kernel-trusted compositor routing. |
| CIUKI_DOSVM_FOCUS (4) | Flag + owner | own-session designated focus path; no authority over another VM. |
| CIUKI_DOSVM_CLOSE (5) | Flag + owner | own-session owner; SYSTEM session teardown. |
| CIUKI_DOSVM_STATUS (6) | Flag + owner | own-session owner. |
| CIUKI_DOSVM_LAUNCH (7) | Flag + owner/ready VM | own-session; guest file/EXEC paths also authorized. |
| CIUKI_DOSVM_PRESENT_BANDS (8), redraw/scanline choices | Flag + owner | own-session owner/surface. |
| CIUKI_DOSVM_FULLSCREEN (9) | Flag + focused owner/host display handoff | own-session presentation lease; guest never receives host disk/device authority. |
| CIUKI_DOSVM_BACKGROUND (10) | Flag + owner | own-session VM execution preference. |
| CIUKI_DOSVM_WAIT_PRESENT (11) | Flag + owner | own-session; revoked/cancelled at logoff. |
| CIUKI_DOSVM_GAME_QUERY (12) | Public game presence/bytes query | anyone for machine catalogue/nonsecret installation state. |
| CIUKI_DOSVM_GAME_REMOVE (13) | programs_capability | administrator; same active-VM exclusion/transaction. |
| CIUKI_DOSVM_SAVE_QUERY (14) | Public save-state query | own-session current uid's save tree; no other user's names/usage disclosure. |
| CIUKI_DOSVM_SAVE_RESET (15) | programs_capability, idle title | own-session current user's saves and confirmation; never machine-wide implicit deletion. |
| CIUKI_DOSVM_SAVE_EXPORT (16) | programs_capability, idle title | own-session current user's saves; destination must also pass path policy. |
| CIUKI_DOSVM_IMAGE_QUERY (17) | Public verification state | anyone; does not confer image modification permission. |
| CIUKI_DOSVM_TEXT (18) | Flag + owner | own-session; no cross-user screen text. |
| CIUKI_DOSVM_MARK (19) | Flag + owner | own-session. |
| CIUKI_DOSVM_CLIP_REQUEST (20) | Flag + owner/transient request | own-session clipboard namespace. |
| CIUKI_DOSVM_CLIP_COMPLETE (21) | Flag + owner/matching serial | own-session; reject stale reply after logoff. |
| CIUKI_DOSVM_TEXT_CHUNK (22) | Flag + owner | own-session. |
| CIUKI_DOSVM_POLICY (23) | Flag + owner | own-session bounded VM preference; no host policy write. |

### Win32 transport (call 96), including imported ABI families

The additional records in w32_transport.h, w32_core.h, present_abi.h and
w32_diagnostics.h are included here because execution-abi.md makes their
actions normative even though some declarations live outside abi.h.
Current family dispatch: proc/win32/syscall.c:30–120; validation:
proc/win32/records.c. Unless noted, current authorization is Win32
personality plus owned handles and the family's narrower checks, without
uid/session security.

| Every family/action | Current authority / file:line evidence | Proposed class |
| --- | --- | --- |
| W32_IDENTITY (0x1), W32_COUNTER (0x2) | Win32 self/info; abi.h:3307–3308; proc/win32/syscall.c:75–81 | anyone within supported personality, self identity; immutable credential queries use SECURITY/token transport. |
| W32_VM_ALLOC (0x3), VM_FREE (0x4), VM_PROTECT (0x5), VM_QUERY (0x6) | Own Win32 VA; abi.h:3309–3312; proc/win32/syscall.c:82–88 | own-session self; allocation/protection flags never bypass file/view rights. |
| W32_DISPATCHER (0x7), CONTINUE (0x8), RAISE (0x9), THREAD (0xA), DIAGNOSTIC (0xB) | Own validated executable/context/thread/report; abi.h:3313–3317; proc/win32/syscall.c:89–120 | own-session self; malformed CPU state stays contained under the existing exception contract. |
| W32_OBJECT (0x100): CREATE, OPEN | Named/owned Win32 object or process/thread handle; w32_transport.h:6, :71–74; execution-abi.md:2198 | own-session object namespace; acquiring another uid's nonprotected process handle requires administrator and explicit requested rights; SYSTEM objects remain protected. |
| W32_OBJECT: CLOSE, DUPLICATE, INHERIT, SET_INHERIT | Handle/source/target process handles; w32_transport.h:74–75, :81–82 | own-session and rights intersection; cross-credential duplicates/inheritance denied, even via an existing source handle. |
| W32_OBJECT: SET, RESET, PULSE, RELEASE, QUERY, TIMER_SET, TIMER_CANCEL | Object kind/ownership; w32_transport.h:76–84 | own-session and granted access; named-object collision cannot cross sessions. |
| W32_WAIT (0x101): multiple wait, wait-all, alertable, signal-and-wait, own queue mask | Retained handles/current thread; w32_transport.h:7, :87–99; execution-abi.md:2199 | own-session owned objects; signal side requires modify right, not only wait right. |
| W32_APC (0x102): QUEUE, REGISTER, RETURN | Win32 mapped dispatcher/outstanding context; w32_transport.h:8, :101–107; execution-abi.md:2200 | own-session; APC target rights checked, no queue into another uid/SYSTEM. |
| W32_CONTEXT (0x103): SUSPEND, RESUME, GET, SET | Same process and suspended boundary; w32_transport.h:9, :108–120; execution-abi.md:2201 | own-session same process; administrator does not expand supported debugger scope. |
| W32_MESSAGE (0x104): POST, GET, SEND, REPLY, POLL_REPLY, STATUS | Win32 queues, receiver-bound reply tokens; w32_transport.h:10, :122–127; execution-abi.md:2202 | own-session window station; sender/target session checks before payload copy/delivery. |
| W32_MESSAGE: SELECT, MARK, CANCEL, NOTIFY, QUIT, START | Own queue/send token; w32_transport.h:135–140; execution-abi.md:2620–2655 | own-session; cannot mark/reply/cancel another session's token. |
| W32_SECTION (0x105): creation, PAGEFILE/FILE/IMAGE sources | Win32 object/file handle rights; w32_transport.h:11, :49–51, :146–152; execution-abi.md:2203 | own-session and source VFS policy; SYSTEM or admin source fd cannot be laundered into user-readable shared pages. |
| W32_VIEW (0x106): MAP, UNMAP, FLUSH | Own section/view, access flags; w32_transport.h:12, :153–164; execution-abi.md:2204 | own-session; retain source subject, max rights and revocation generation through flush/writeback. |
| W32_LOADER (0x107): OPEN, IMAGE, UNMAP; EXACT/BUILTIN/DATAFILE flags | Win32 immutable snapshots and image arena rules; abi.h:3539–3546; execution-abi.md:2343–2373 | own-session; OPEN checks path permission even on retained/cache hit; BUILTIN cannot rewrite or impersonate trusted SYSTEM image. |
| W32_WORD (0x108): wait/wake | Own private user word; abi.h:3540; execution-abi.md:2375–2380 | own-session self; no cross-process/private-word address alias. |
| W32_FILE (0x109): OPEN, READ, WRITE, SEEK, TRUNCATE, FLUSH, STAT | Win32 file handles/access/share and System-tree restriction; w32_core.h:6, :10–17; execution-abi.md:2425 | own-session plus VFS policy and retained description; raw device names never bypass device authorization. |
| W32_FILE: ATTR, TIMES, DELETE, MOVE, MKDIR, RMDIR, CHDIR | Path/attribute/share checks; w32_core.h:18–24 | own-session plus resolved object/parent/metadata/move policy. |
| W32_FILE: CWD, FULLPATH, FIRST, NEXT, DISK | Caller cwd/find state/volume metadata; w32_core.h:25–29 | own-session query of accessible paths/find entries; anyone nonsecret volume geometry. |
| W32_FILE: LOCK, UNLOCK | VFS range owner; w32_core.h:30–31; execution-abi.md:2659–2664 | own-session retained file identity; lock ownership never substitutes for user authorization. |
| W32_SYSTEM (0x10A): TIME, MEMORY, YIELD, POWER | Win32 public clock/memory/power/current-thread yield; w32_core.h:7, :53–80; proc/win32/system.c:44–81 | anyone nonsecret snapshots; self yield, no machine mutation. |
| W32_SYSTEM: TERMINATE_PROCESS, TERMINATE_THREAD, THREAD_EXIT | Held process/thread handle, self fallback; w32_core.h:56–58; proc/win32/system.c:82–116 | own-session/same uid for other nonprotected targets, administrator across users, SYSTEM protected services; self exit unchanged. |
| W32_SYSTEM: THREAD_PRIORITY, PROCESS_PRIORITY, QUERY_PRIORITY | Handle/priority validation; w32_core.h:60–62; proc/win32/system.c:117 | own-session permitted normal/idle settings; administrator for any already-supported HIGH request; unsupported REALTIME/TIME_CRITICAL stay rejected. |
| W32_SYSTEM: READ_MEMORY, WRITE_MEMORY | Handle must name self; w32_core.h:63–64; proc/win32/system.c:21–26; execution-abi.md:2665 | own-session self only; no new debugger or SYSTEM memory access. |
| W32_SYSTEM: TIME_ZONE | Same call-97 time_capability check; w32_core.h:65–69; proc/win32/system.c:48–60 | anyone GET; administrator setters/acknowledge. |
| W32_PROCESS (0x10B): CreateProcess/startup/inherit/suspended creation | Existing atomic PE preparation and handle inheritance; w32_core.h:8, :93–99; execution-abi.md:2427 | own-session inherited credential; all startup/image/cwd/handle fields checked as child; no elevation by EXE name. |
| W32_SNAPSHOT (0x10C): create(1), first(2), next(3) | Held immutable Toolhelp snapshot, process/thread/module collection; w32_core.h:9, :100–110; proc/win32/records.c:128–131; proc/win32/snapshot.c:65 and :129 | anyone nonsecret process/thread summaries; module enumeration remains self-only (the existing narrower supported scope); private paths require own-session rights. Administrator status does not add unsupported remote module walking. Filter before storing snapshot. |
| W32_COMPAT (0x10D): action 1 snapshot digest/identity, action 2 copied settings | Own original executable snapshot/shim settings; w32_diagnostics.h:5; execution-abi.md:2569 | own-session self; global AppCompat override writes administrator, immutable builtins SYSTEM. |
| W32_REPORT (0x10E): CRASH(1), REFUSE(2), REASON(5) | Caller report/restart reason; w32_diagnostics.h:29–30; execution-abi.md:2570; proc/win32/diagnostics.c:156–159 | own-session self; reason is not permission to restart other users or the machine. |
| W32_REPORT: NEXT(3), CLOSE(4) | Delegated desktop PID; w32_diagnostics.h:29–30; proc/win32/diagnostics.c:143–145 | own-session compositor and faulting target; administrator can request permitted Task Manager termination but cannot silently dequeue another session's registers. SYSTEM may collect trusted diagnostics. |
| W32_REPORT_NATIVE_FAULT (6) | Result kind, explicitly not action; w32_diagnostics.h:26–30 | No callable grant. Native crash records carry the same session privacy as Win32 records. |
| W32_SERVICE (0x200): REGISTER, ACCEPT | Exact path-delegated native role; w32_transport.h:13, :165, :167; proc/win32/service.c:45–57 and :182 | SYSTEM assigns session service role; own-session registered owner operates it. Public REGISTER cannot create the role. |
| W32_SERVICE: CONNECT | Win32, or exact native desktop/control exception; w32_transport.h:166; proc/win32/service.c:215–226 | own-session authenticated peer to the correct service; native programs can request only supported service operations, subject to credentials. |
| W32_SERVICE: PRIORITY | Own normal/idle, high/realtime/time-critical refused; w32_transport.h:168; proc/win32/service.c:243–261 | own-session and same restrictions; no SYSTEM privilege via priority. |
| W32_SERVICE: FOCUS, INPUT | Registered live desktop; w32_transport.h:169–170; proc/win32/service.c:262–283 | own-session compositor to a verified same-session client/TID. |
| W32_DISPLAY (0x201): GRANT, REVOKE | Desktop/accepted client link; present_abi.h:6–8; execution-abi.md:2313–2328 | own-session compositor to same-session connected client, exact pid/token/generation. |
| W32_DISPLAY: FRAME, PALETTE, GAMMA | Presentation lease holder; present_abi.h:9–11 | own-session holder; revocation/logoff drains/rejects pending uploads. |
| W32_RENDER3D (0x202=514): CIUKI_R3D_QUERY, CIUKI_R3D_OPEN, CIUKI_R3D_CLOSE, CIUKI_R3D_RESOURCE, CIUKI_R3D_FREE, CIUKI_R3D_UPLOAD, CIUKI_R3D_READ, CIUKI_R3D_TEXTURE, CIUKI_R3D_SUBMIT, CIUKI_R3D_WAIT, CIUKI_R3D_PRESENT | Admitted Win32 render context/resource handles; abi.h:3378–3390; proc/win32/syscall.c:73 | own-session context/resources; READ cannot expose another context's pixels; PRESENT additionally needs the session's presentation lease. |

Global/private named Win32 objects and window stations are keyed by security
session, not just name. No Global namespace privilege is introduced. Registry
RPC, profile INI leases (PROFILE_ACQUIRE/RELEASE), compatibility reads,
clipboard/format ownership, shell links, installer Run/RunOnce/WININIT queues,
window-class registration and virtual restart are user-space protocols, not
extra syscalls. They must authenticate the kernel peer/session and obey the
filesystem/registry classes above (execution-abi.md:2233–2245, :2657–2674;
win32-personality-contract.md:1058–1107). A standard user's request cannot
cause SYSTEM to write machine settings or execute an installer as SYSTEM.
Virtual restart affects only that session's Win32 processes and permitted
queues. It cannot promote an HKCU Run entry into machine startup.

Library-only helpers such as clock_getres, sbrk, gettimeofday, pthread
mutex/cond wrappers, ELF/libc startup, POSIX/Win32 priority translations and
SIGRETURN/NtContinue trampolines use the listed calls with the caller's
credential. Internal storage_enable_write/test probe checkpoints, firmware
leases, driver activation and kernel scheduler boosts are SYSTEM internal
actions, not user-authorizable APIs (execution-abi.md:538–565, :1207–1274,
:2785–2813). A diagnostic/test selector cannot be set from an ordinary process
to obtain those internal rights.

## Host test plan and implementation directives

No host test is needed to establish this design: the current authorization
gaps above are visible in the inspected production branches. The tests in
this section are future acceptance obligations, not claimed results.
The lead owns cross-review and writes the final implementation directives.
Run every host suite serially with nice -n 19, preserve repository ASan/UBSan
and leak-detection settings, and retain exact commands/results in each report.
No QEMU, systemd scope or image build is part of these implementer directives.

| Proposed directive / dependencies | Host-testable implementation scope | Acceptance criteria and meaningful adversarial cases |
| --- | --- | --- |
| sec-k01: credential/process ABI; joint Q2 freeze | Immutable credential/session primitives, SYS_SECURITY query/create/spawn/end, libc queries, independent ABI oracle, kernel/SDK copies | Production proc_prepare/spawn/thread/Win32 spawn/exec replacement inherit exact uid/groups/session; adoption and arbitrary path/env/authority fd cannot elevate. SYSTEM alternate spawn publishes only after full validation; fault every allocation/copy/descriptor step. Test uid/group/session overflow, zeroed reserved fields, group sorting, group-set overflow, stale pid/peer and protected-token query denial. All size/alignment/offset/constant oracle rows pass i686/native adapters; generate API reference. |
| sec-k02: managed volume policy and owner store; k01, Q1/Q3 | Shared explicit-subject VFS API, mount binding, bounded policy trie, owner provisioning/recovery, denied-by-default missing subjects | Same policy result through native, DOS-style VFS and Win32 paths. Test every rule with SYSTEM/admin/standard/other-user/Guest, LFN+8.3 alias, ASCII/non-ASCII case, /mnt aliases, relative cwd, dot/dotdot, prefix sibling, Win32 trailing dots/spaces, malformed UTF-8/LFN, missing policy/owner map and injected allocation/I/O failure. Corrupt/new/backup/journal combinations never expose a profile or recycle a uid. |
| sec-k03: all file/VM/mapping enforcement; k02 | Open/create/read/write/pread/pwrite/stat/getdents/exec/snapshot/mkdir/rmdir/rename/unlink/truncate/attrs/times/sections, shared caching and workers | A direct lower-level production API cannot bypass checks. Use a spy backend to prove denied calls issue zero writes/reads of private data. Race namespace relabel/move, replacement, slot reuse, policy generation, retained fds/views, deferred delete, queued writeback and unmap; same-root rename succeeds, cross-root unauthorized rename is atomic refusal, unrevokeable map returns EBUSY. Cached executable hit still denies a different user. Retained buffers/charges are released on every fault. |
| sec-k04: administration/devices; k01/k02, Q4 | Remove path booleans as privilege, credential checks for every inventory device action, raw disk reads/writes/partition lease, media/recovery/network/time/power/display | Table-driven coverage includes every action above and unknown action/version. Standard user's exact /Programs/control, terminal or setup never grants admin rights; env/--authority-fd spoofing fails. Raw protected sectors cannot reveal verifiers/pagefile, nonboot partition acquisition requires admin, inherited raw fd rechecks caller, writes still require quiescent exclusive lease. Own-session media/audio/display/input work; other-session takeover is refused. Probe actual production dispatch, including keyboard and Setup-net declaration gaps. |
| sec-k05: DOS/native-save boundary; k03/k04 | VM credential attribution, every file-hook/redirector/FCB/LFN/EXEC path, virtual raw disks, per-user COW/save/print output, clipboard | Run production VM host harness with forged PSP/DFV2/registers/drive/LBA and raw INT13/25/26/block-IOCTL cases; no physical-device callback reachable. Denied bound-drive request returns DOS error 5 without local-FAT fallback. Two users running the same title get separate saves; reset/export targets only caller and destination policy; machine uninstall admin-only. No memory-cap change or NTVDM substitution. |
| sec-k06: Win32/registry broker; k01/k03, Q5 | Kernel token queries, session object/process/window namespaces, authenticated w32srv subjects, HKCU/HKU rebinding and machine-hive broker, installer queues | Actual production Win32 dispatcher and service backend tests, not only facade mocks: standard/Guest cannot mutate HKLM via key handles, raw hive files, imports, INI/shortcut/RunOnce helpers or forged uid fields. HKCU/private roots cannot address another uid. Cross-session named object, APC, message/reply, context, process handle/terminate, dup, snapshot/module paths, crash mailbox and pixels are denied. Admin machine installer succeeds; standard per-profile install succeeds or reports a real unsupported engine, never elevation. Existing self-memory restrictions stay in force. |
| sec-k07: logon/session integration and recovery; k01–k06, joint user contract | SYSTEM account service/bootstrap/secure UI, revocation/drain, logoff, saver lock boundary, Setup/custom-user and ready-image account manifests | End-to-end host state machine uses production process/VFS/service/VM teardown: save veto leaves same session; confirmed logoff blocks spawn/queued requests and ends orphans/VMs, drains issued I/O, destroys handles/maps/clipboard/fault replies and prevents stale-worker access to the next session. Failure to fence blocks admission without panic. SYSTEM service crash releases lock/recoverable journal; password/entropy/OOM/disk faults never create a default anonymous session. Safe Mode still needs uid 500's valid password; disabled Guest cannot log on. |
| sec-k08: coverage/performance close; all above | Inventory-to-dispatch audit, bounded warm-path counters/benchmarks and ledger regressions | An independent source-derived list of syscall/subaction selectors maps to one policy test each; adding a selector without a policy row/test fails the audit. Denied access cannot be bypassed via alternate native/DOS/Win32/private-worker route. Warm fd operations perform zero new storage lookups and bounded constant-time checks; document metadata/high-water deltas at process/handle/root limits. Target KDF/PIII latency and physical/runtime qualification are explicitly left to the lead. |

Existing host suites supply reusable production seams: the ABI section of
scripts/test/host_kernel_tests.sh; tests/host/proc/disk_test.c for disk fd
dispatch; tests/host/net/settings_test.c and settings_run.py for network
control; tests/host/win32/registry_test.c, registry_facades_test.c and
test_registry.py for registry services; tests/host/desktop/users_test.c and
test_users.py for G32 formats; tests/host/fs/test_registry_commit.c for
transaction recovery. They are regression inputs, not proof that the future
security rules are already covered. The lead must name the exact new harness
commands in each directive; inventing executable test names here would not
constitute verification.

Acceptance must distinguish authorization failure from I/O failure: seed
single-error cases for EACCES/EPERM/EBADF/ESRCH and preserve positive partial
counts after issued I/O. Exercise both returned errno and unchanged external
state. Fault injection must cover credential/endpoint release, root owner
publication, disk fencing, pending logoff, profile recovery and the last
descriptor/view reference. Sanitizer/ledger checks must prove no retained
secret buffer, fd, pin, process, mapped page or VM survives successful teardown.

## Open questions and lead decisions needed

None of these items overrides the shared identity/credential/FAT/account
decisions. Recommended choices are proposals for cross-review; unresolved
choices block the listed implementation slice, not this design deliverable.

| Id | Decision needed / recommendation | Blocks |
| --- | --- | --- |
| Q1 | Agree with the other user contract on canonical profile/account paths and G32 migration. Recommend retaining /Windows/Profiles/u%08x for private data, /System/Security for SYSTEM metadata, and explicitly migrating existing default /Documents/shared settings; do not silently assign all legacy data to the next logon. Define handling of legacy profile ids below 1000 and prevent collision with 500/501; preserve data and retire mapped ids permanently. | k02, k07 |
| Q2 | Jointly reserve SECURITY syscall/operations, complete QUERY_PEER fd representation and the profile descriptor, flags/errors/group bounds and read-only Win32 token/SID facade. This draft's 64-byte records are proposals, not an ABI freeze. Current W32_DISPLAY is under call 96, despite the old "101" prose. | k01, k06 |
| Q3 | Approve root Creator Owner side records, 32-rule/32-profile/32-root-directory bounds and the explicit shared-data/Guest/fixed-volume matrix. A read-only root is a simpler alternative but differs from the cited XP root baseline. Define additional managed data roots without turning unknown fixed disks into unrestricted system aliases. | k02 |
| Q4 | Approve the SYSTEM-only sealed boot/service set and raw managed-system-disk read restriction. Admin raw reads otherwise defeat the shared SYSTEM-only verifier rule. Keep conservative refusal until an exact readable-sector classification exists. Also decide whether to retain Win32's current extra all-/System write denial or replace it with the common prefix policy; no silent divergence. | k04; machine installer compatibility |
| Q5 | Agree on per-user w32srv plus SYSTEM machine-registry broker, native per-profile roots and protected hive backing paths. A SYSTEM w32srv alternative would need operation-by-operation explicit client attribution across files, hives, helpers and deferred work; never just set the whole desktop to SYSTEM. Coordinate HKU/HKCU ownership, .DEFAULT and profile INI leases. | k06 |
| Q6 | Close historical-source gaps with original XP setup security templates/edition-specific documentation if exact default parity is required. The inspected primary material establishes mechanisms and major folder/HKLM rules, but not every HKU exception, XP lockout default or Welcome-screen filtering rule. Propose zero persistent lockout with bounded backoff; jointly fix queue/backoff constants. Do not cite a later Server default as XP proof. | account policy freeze; exact-default historical claims |
| Q7 | Specify how ready-made images provision CiukiAdministrator's verifier/password while disabled: Setup chooses it, ready images bypass Setup and must not ship an undisclosed shared password or enable empty-password recovery implicitly. Also agree empty-password rules for custom accounts and how authenticated recovery behaves if all verifiers are corrupt. No bypass is implemented by this draft. | ready-image/recovery provisioning in k07 |
| Q8 | Confirm local Guest power permission and per-session mixer/media/display preferences versus persistent machine defaults with the other contract part. Audit existing INPUT_KEYBOARD_SET forwarding and CIUKI_SETUP_NET_QUERY routing before claiming device acceptance. Reject unsupported paths until the respective implementation directive fixes them. | k04, user-facing permissions matrix |

Cross-review, the final implementation directives, integration, commits and
runtime/hardware qualification remain the lead's work. No code, build,
QEMU run, host runtime test or security-performance measurement is delivered
by this contract.
