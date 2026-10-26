# User security: kernel enforcement contract

Reconciled 2026-10-26 after running `date`; inspected worktree HEAD:
`7f31324b8f2099ee8ade98b3601b12e25857e058`. Authority:
[sec-kernel](../directives/sec-kernel.md),
[sec-contract](../directives/sec-contract.md) and the binding
[lead answers](../directives/sec-lead-decisions.md). M-Q1..M-Q7 identify
model answers; K-Q1..K-Q8 identify kernel answers. This specifies future
implementation, not delivered security. The coordinated
[Implementation plan](user-security-contract.md#implementation-plan) is the
single directive/dependency plan for both halves. No source or public header
changes accompany this document.

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
| Ready-made image, including raw/QEMU/VirtualBox/VMware distribution | Initially CiukiUser uid 1000, Administrators+Users, empty password, direct logon only as the sole enabled interactive account. A1-1 permanently retires this exception before another account may be enabled, or CiukiUser renamed/demoted unless already disabled. |
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
PID 1 with explicit noninheritable kernel roles. Public spawn of even the exact same path cannot
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

Round 1: A1-1..A1-11 in the final Decisions table implement
[Lead amendment 1](../directives/sec-contract.md#lead-amendment-1-2026-10-26-1145-cross-review-round-1).
The main half's final delta lists all interfaces changed since 3ddc9de88 for the
already running sec-k01/sec-user-01 implementations. The three original ABI
records keep their sizes/offsets; operations 8..14 and three new records make
launch, secure console and role-bound connections explicit.

## Primary research and comparison

These sources were consulted on the contract date. General API pages describe
mechanisms; their modern defaults are not treated as XP installation defaults.
Each numbered conclusion below is followed by the CiukiOS decision it informs.

| Ref | Primary source and finding | Decision |
| --- | --- | --- |
| R1 | Microsoft [well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids) distinguishes account RIDs, builtin groups and logon identities. | Use the lead's numeric uid/group assignments. These numbers are not complete Windows SIDs and uid 0 is not RID 500. |
| R2 | Microsoft [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens) associates user, groups and logon identity with processes; [privilege constants](https://learn.microsoft.com/en-us/windows/win32/secauthz/privilege-constants) distinguishes system privileges from object rights. | Immutable process credentials; separate resource ownership from administrator membership. No thread impersonation in this step. |
| R3 | Microsoft's [MS02-064 XP root baseline](https://learn.microsoft.com/en-us/security-updates/securitybulletins/2002/ms02-064) permits root folders and creator ownership; root file creation is inherited below root. | M-Q7 adopts creator-owned root folders and explicitly extends the rule to root files for old installers. Protected anchors/other owners are not writable. |
| R4 | Microsoft [gaming with least-privileged accounts](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/gaming-with-least-privileged-user-accounts) gives XP NTFS folder defaults, FAT32's lack of security, read-only HKLM and per-user HKCU storage. | Protect OS/program paths, permit own profile data, provide explicit shared documents, and enforce the policy on FAT ourselves. Games patch machine installs as administrator. |
| R5 | Microsoft [registry key security](https://learn.microsoft.com/en-us/windows/win32/sysinfo/registry-key-security-and-access-rights) checks requested key rights and inherits key security from the parent; [registry structure](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users) distinguishes HKLM, HKU and HKCU. | Authorize each registry operation against its authenticated client and namespace; protect entire hive backing files against bypass. |
| R6 | The Microsoft [Windows XP Security Guide, chapter 3](https://learn.microsoft.com/ko-kr/security-updates/security/20213971), retained in Korean with English policy names, assigns shutdown to Administrators+Users and clock/driver/volume maintenance to administrators in its client configurations. These are configuration recommendations, not proof of every XP default. | Allow local interactive shutdown/restart; reserve machine configuration and maintenance for administrators. |
| R7 | Microsoft [SetTimeZoneInformation](https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-settimezoneinformation) explicitly requires SeSystemtimePrivilege on XP/2000; the separate time-zone privilege applies to later Windows. | Both system clock and system time zone require administrator. Per-process TZ conversion can remain unprivileged. |
| R8 | Microsoft [format/eject removable media policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/jj852171(v=ws.11)) includes XP and distinguishes use of media from format/eject rights; [media-burning device permissions](https://learn.microsoft.com/en-us/windows/win32/imapi/providing-user-permissions-for-media-burning-devices) identifies XP's extra device permissions. | Ordinary file use is per-session. Formatting/raw snapshots require administrator. Allow safe removal/tray use to the local session as an explicit CiukiOS usability choice. No media-burning permission is implied. |
| R9 | Microsoft [KB814777](https://learn.microsoft.com/en-us/troubleshoot/windows-client/windows-security/access-computer-after-administrator-disabled) documents Safe Mode access to a disabled local administrator in its server/RIS scenario. [Advisory 906574](https://learn.microsoft.com/en-us/security-updates/securityadvisories/2005/906574) explains XP Guest/ForceGuest network behavior. | Follow the owner's explicit CiukiAdministrator/Guest defaults, not an assertion that every XP edition disabled Administrator or that Guest visibility is equivalent to network authentication. |
| R10 | Microsoft-authored XP Resource Kit [Authentication Policy Options](https://flylib.com/books/en/2.922.1.130/1/) (mirror) states that Local Security Policy workstation lockout is disabled by default. | K-Q6 fixes no persistent lockout plus the model's one-second backoff. No exact XP template parity is required. |
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
behavior. K-Q6 closes these historical-template gaps as nonblocking: the
normative access matrix documents CiukiOS choices rather than exact XP parity.
The Microsoft-authored XP account/Welcome material and lockout evidence in the
main contract's R3/R4/R8 were rechecked during reconciliation. Microsoft
[logon-screen shutdown policy](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/jj852274(v=ws.11))
distinguishes client logon-screen power from authenticated user rights; K-Q8
explicitly admits Guest too. [NIST's historical password guidance](https://pages.nist.gov/800-63-3/sp800-63b.html#memsecretver)
is identified as superseded; the approved 8-63-byte bound is a CiukiOS choice.

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
particular, the old execution-ABI display amendment incorrectly said "Call
101"; this reconciliation corrects it to W32_DISPLAY under call 96, matching
abi.h:769 and proc/win32/syscall.c:73–74. The distinct new SECURITY call 101
follows the allocation rule; it does not rename or move the Win32 transport.
The "90 reserved ENOSYS" prose at lines 2678–2683 is also superseded by the
implemented call-90 amendment at lines 2725–2733.

## Process credentials, sessions and trusted services

### Credential lifetime

Each process MUST retain one immutable credential value before publication:
uid, primary group, bounded sorted unique supplementary group set, security
session id and kernel-validated flags. Eight group slots are fixed;
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

Only the kernel-established SYSTEM LOGON role may use the frozen
SPAWN_CREDENTIAL operation. It prepares a
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
this path. PID 1 bootstrap assigns sealed service roles; session desktop/w32srv launch
uses the explicit SESSION_LAUNCH operation below, never an arbitrary executable
path or a role inherited from a parent.

Account role/password changes update the database for future logons.
Existing credentials remain immutable. Role/type changes, disablement and
deletion of the active account return EBUSY before writes; log off first, then
retry. SYSTEM session termination handles crash/recovery, not in-place token
editing. This adopts the model half's conservative active-account rule (M-Q3).
Password change alone need not revoke the owner's active session. Only SYSTEM may add/remove groups in the database;
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

After account/password verification, ACCOUNT sends a retained-link result to
LOGON, which creates the session and uses SESSION_LAUNCH/LAUNCH_POLL for its
desktop and w32srv. Only LOGON may create/spawn/end sessions or hand off the
secure console; uid 0 alone does not qualify. The logon
UI and account service are separate from that desktop. A child of an
administrator is an administrator automatically, including an installer.
A standard user's child remains standard even if named setup, control or
w32srv. Ready-image autologon uses the same session path with CiukiUser's
explicit empty-password verifier and persisted autologon choice, only while it
is the sole enabled interactive account. A1-1 in the main contract permanently
retires this exception on password/disable/replacement/name/type transitions;
enabling another account while it is blank/enabled is refused, including Guest.
Reboot never restores retired autologon.

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
   the destination uid. The secure UI takes exclusive input/display as soon as
   CLOSING begins.
   Only after teardown is complete does the session become DEAD and the UI
   permit a fresh logon; it never leaves a draining desktop in control.

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
list includes the primary gid and is sorted: SYSTEM [0], administrator
[544,545], Standard [545], Guest [546]. Identity
setters are unsupported/EPERM, including requests from administrator uid 500;
they do not become setuid gateways.

Win32 OpenProcessToken/GetTokenInformation/CheckTokenMembership must query
the kernel credential, not an advapi32 or desktop cache that a process can
edit. TokenUser, TokenGroups and session queries are read-only views; requests
to create/assign/adjust credentials are refused. A query token handle is not
an authority transferable to CreateProcessAsUser. The SID/token facade and
privilege view below are frozen with the transport under K-Q2/M-Q3; they cannot change credentials.

QUERY_PROCESS permits an administrator to query other non-SYSTEM processes,
or a process to query its own uid in its own session; SYSTEM can query all.
It returns no password material, environment, command line or file contents.
QUERY_PROCESS on protected PID 1/services requires SYSTEM. Public process
summaries and direct QUERY_PEER on an owned broker link may disclose only the
public SYSTEM credential, without private process details. Regular process listings remain bounded public diagnostic summaries; private
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
Additional fixed volumes are managed data volumes with the same creator-owner
root rule (K-Q3), never aliases that shed the system volume's protection.
Existing ordinary entries without owner records are public-readable and
administrator-writable; new Standard-created root files/folders require a
committed owner record. Missing/corrupt managed metadata denies new root
creation and access to ownership-classified data; it cannot relax policy.

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

### Rule table

Paths below use the native system-volume spelling. G32's numeric profile
layout is the canonical new PROFILE(uid) = /Windows/Profiles/u%08x (K-Q1).
Imported numeric roots may retain their old spelling through explicit
old-id -> new-uid owner mapping; no suffix is an authority or alias for 500/501.
"Read" includes lookup/stat/enumeration and loading executable bytes when the
object is executable by existing format rules. "Modify" includes create,
write, delete, rename, truncate, attributes and timestamps, subject to the
separate parent rules. All ordinary accesses still obey FAT read-only bits,
share/range locks, mount state and the device lease rules.

| Prefix / object | Standard Users | Guest | Administrators | SYSTEM / special rule |
| --- | --- | --- | --- | --- |
| Every managed fixed-volume root | Read; create new ordinary files and folders, creator full control; other Users read/execute | Read only | Full ordinary-data control | M-Q7/K-Q3: commit creator metadata before publication; sealed installed anchors cannot be replaced. |
| /Windows, /Windows/System, /Windows/System32, /Windows/Fonts | Read | Read | Read/modify application/shared files | More specific sealed/security/profile rules win. No global writable TEMP exception. |
| /System and /Programs, /Program Files, Common Files, bundled game binaries | Read | Read | Read/modify ordinary program content | Protect the sealed executable/boot-input subset separately. K-Q4 replaces the extra Win32 all-C:/System write denial with this common prefix policy; F5 builtin-store protections remain. |
| Kernel/boot inputs, SYSTEM service images and security policy manifests | Read executable/nonsecret content only | Same | Read only | SYSTEM maintenance only; path ancestors cannot be renamed or replaced. Includes files consumed as privileged code/configuration during the next boot. |
| /System/Security, account verifiers, owner map, security journal/backup/temp/locks | No access | No access | No direct access | SYSTEM only. Parent enumeration must not disclose file contents; account management uses the authenticated broker. |
| /Windows/Profiles container | Enumerate nonsecret account directory names, traverse only an allowed profile | Same | Read | SYSTEM creates/removes/relocates profile root anchors. A user's name is not their ownership proof. |
| PROFILE(uid), including Documents, Desktop, Favorites, Start Menu, application data, settings and Temp | Owner read/modify; others no access | Only Guest's own profile | Read/modify other profiles' ordinary data | SYSTEM maintains root ownership. Security subtrees remain SYSTEM-only even to a profile owner. |
| Default profile template | Read only | Read only | Read/modify template data | SYSTEM creates each private profile by copy under its new owner. The template never creates an anonymous session. |
| All Users/common Start Menu and desktop | Read | Read | Read/modify | Shared executable launch configuration cannot be changed by ordinary users. |
| /Programs/Installers and bundled desktop reference anchors | Read/execute | Read/execute | Read/modify | Protected installer namespace and ancestors, absolute shortcut target/cwd, no user DLL planting; main section 8.3 inventory and sec-user-02 build migration. |
| /Documents (legacy default/shared documents) | Read/modify shared data except protected presentation anchors | Read only | Read/modify | Explicit shared-data choice; never the automatic private Documents path for uid 1000. No SYSTEM service executes code/configuration from here. K-Q1 keeps it shared; migration never assigns it to the next logon. |
| /system/settings and /System/Registry machine settings | Read nonsecret values; no direct modification | Same | Read/modify ordinary machine settings | Security records excluded; Hardware is read-only inventory; broker-owned hive/journal files excluded. Private Desktop/Users/Programs preferences move under PROFILE(uid); authoritative machine-registry files are broker-only. |
| Personal restart journal at PROFILE(uid)/Settings/Restart and machine restart journal under /System/Security/Restart | No direct access | No direct access | No direct access | SYSTEM broker writes only authenticated jobs; same-user replay, machine admin authorization, no .DEFAULT/next-user inheritance; main section 6.2. Protect anchor ancestors. |
| Authoritative registry hive backing files, transaction slots and journals | No direct access | No direct access | No direct access | SYSTEM registry broker only, including personal hive persistence under PROFILE(uid)/Settings/Registry; session w32srv uses scoped requests. Old SYSTEM.CRG/USER.CRG are protected migration inputs. |
| Non-authoritative per-profile native libreg preferences outside Settings/Registry | Owner read/modify | Owner only | Read/modify | May contain only that user's preferences, never account roles, machine policy or another user's data. |
| Pagefile, disk/security logs and retained crash-register reports | No direct read/write | No direct read/write | Read nonsecret diagnostics via API; no raw backing access | SYSTEM-owned; stale paging bytes must not be readable through raw disk or file opens. Per-user crash reports may instead be placed in the owning profile. |
| Additional fixed-volume existing ordinary data | Read/execute; new root entries follow creator rule | Read only | Read/modify | Same root policy and 256-entry owner bound per volume; never import authority from foreign FAT labels/maps. |
| Unmanaged removable FAT/USB/floppy | Local active session read/modify ordinary files | Read only | Local active session read/modify | Shared media has no cross-user confidentiality promise; no trust in foreign owner maps, autorun or service registration. |
| CD/ISO, read-only NTFS and write-protected media | Read | Read | Read | Mutations remain EROFS for every identity. |
| VM private logical disk/COW and saved game overlay | VM owner's session only | Own VM only | Own VM; other users' saved data only through explicit administrative file management | Kernel provider mediates sectors. Persistent saves are under the owning profile, not a machine-global writable COW. |

Creator-owned root entries are ordinary files or folders, up to 256 per
volume, with ownership inherited by descendants. The creator and administrators
may modify/delete their ordinary content; other Users read/execute, Guest
read-only. Read/execute is not permission to modify an unowned child. Installed
system/boot/profile anchors remain protected even from their nominal creator.
Unlike the former kernel draft, these roots are shared-readable, not private;
private data belongs in PROFILE(uid). Unlike the former model draft, Users may
create `C:\GAME` and root files. Microsoft's R3 baseline allows root folders;
root files are the lead-approved compatibility extension, not exact XP parity.
Every creation/rename/deletion commits its bounded side record atomically with
namespace publication or remains inaccessible pending recovery. Exhaustion
returns ENOSPC before any directory/file appears; no fallback to an ownerless
writable entry. No general NTFS ACL inheritance is introduced.

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

Only SYSTEM provisioning may create/remove/rebind a profile ownership anchor.
User rename/delete of a profile or hive anchor, sealed prefix, its ancestor
or managed-volume alias is refused. Creator-owned ordinary root files/folders
can be renamed/deleted by their owner or administrators, with the kernel
transactionally updating/retiring the side record and fencing retained handles;
this exception does not permit changing the recorded uid. Renaming content
inside an ownership root is permitted if both sides authorize it. Profile renames update the
account display name, not the numeric storage root.

Use bounded records, checksums, generation and the registry contract's
intent/new/backup durability protocol; checksum is corruption detection,
not authentication against physical tampering. Provision in a deny-first
state, fsync metadata and directories/volume, then publish the account/root.
On uncertain recovery, keep the affected root inaccessible and retain the
old committed metadata. A stale/missing owner map must never turn a profile
into publicly writable data. Exhausted bounds return ENOSPC/ENOMEM. Mounting
or logon can enter authenticated recovery; no new panic or default profile.
The account/owner PREPARE/COMMIT/ABORT/STATUS handshake, durable uid reservation
and cross-store recovery order are in the main contract section 3.5. The
kernel-registered OWNER endpoint is service 3 with ACCOUNT/boot-SETUP role
checks, full batch codec, durable receipts and STATUS retention in main section
3.5; no per-binding generations or claimed uid authorization. It is not a public
credential setter. SESSION_CREATE validates only committed
owner keys/generations through the frozen profile descriptor.
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
| Move between owner/policy roots | Ordinary users receive EACCES even if they can write both roots; use copy then delete with independent checks instead. Administrator/SYSTEM may relocate ordinary content only after destination classification and handle invalidation are reserved. Profile/sealed anchors never move through ordinary rename; the explicit
creator-owned root-entry rename transaction described above is permitted. Different volumes retain EXDEV. |
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

Removable media is shared local data: Users may read/write files; enabled
Guest has read-only access (M-Q7). Both may launch programs as themselves,
use CD audio and request safe removal after the existing blockers/flush checks. No media grants
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

Keep C: private writable FAT16, D: read-only system view and E: removable
exactly as the unified DOS contract and dos_file_vm.c:189-207. C:\WORK/C:\TEMP
are private provider paths; per-user COW/saves persist under PROFILE(uid),
separately from credential-filtered, read-only D: profile export and its existing
8.3 aliases. Do not map C: to the host system volume. E: still obeys its actual
provider/media writability; Guest policy cannot widen it.

INT25/26 on managed C:/D:/E: **fail** CF=1 AX=0201 with the extra FLAGS stack
word preserved; INT21/7305 valid packets fail CF=1 AX=020C, no transfer/buffer
change. Keep the unified contract's invalid-parameter behavior, never relax these
APIs even to a private disk. BIOS INT13, permitted guest block IOCTLs and DPMI
I/O may reach only VM-private logical disks/COW under the admitted provider. A guest drive number,
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

The binding K-Q5 split keeps per-session w32srv under the user's
credential for USER/clipboard/window work and uses a narrow SYSTEM registry
broker for machine hives. The broker authorizes logical HKLM/HKU operations
as the retained client and owns raw machine and authoritative personal hive
files exclusively. Per-profile hive storage is SYSTEM-only at
PROFILE(uid)/Settings/Registry, protected against ancestor rename/removal.
Personal native libreg preference files outside that subtree may be
owner-writable; they contain no authority or machine settings. Direct client
links, or a registered w32srv with the identical session credential, establish
the broker subject; forwarded uid/pid bytes cannot establish authority.
Per-process-sensitive operations require a direct retained client link.
Sec-k06 converts sdk/libreg/registry.c reads, generation polling, mutations and
import/export to the retained REGISTRY adapter; machine backing files are
service-only. Personal non-authoritative preferences keep their owner-writable
backend. Denial/offline errors never create an empty alternate machine root.
Personal versus machine restart journals, v1 quarantine and authenticated replay
are specified in main section 6.2; SYSTEM never executes queued caller commands. The main contract section 6.2 specifies the same split. Running
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
| /dev/ataN raw pread | K-Q4: refuse every user, including administrators, on the whole managed system disk until exact readable-sector classification exists. SYSTEM services perform admitted maintenance after an administrator check. Non-system disk raw reads require administrator and existing device checks. |
| /dev/ataN pwrite, DISK_PARTITION_ACQUIRE | K-Q4: refuse user raw writes on the managed system disk; administrator on non-system disks plus exclusive lease; SYSTEM Setup plus Setup mode for the destination boot disk. Never write mounted/pagefile/log disks. Check the actual caller at each raw I/O even for an inherited fd. |
| DISK_LEASE_ACQUIRE/PUBLISH/RECHECK/SET_PHASE | SYSTEM Setup-only transaction as today; check uid 0 in addition to Setup boot/lease ownership. DISK_LEASE_RELEASE/FLUSH require the actual authorized lease holder. |
| Format/partition/convert/ScanDisk/defrag | Administrator check, plus exclusive volume/device admission; on the system disk only the SYSTEM maintenance service executes the operation. Boot repair runs internally as SYSTEM. Read-only public geometry/status queries remain anyone; scans must not expose private filenames or bypass ACL-like policy. |
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

## Frozen ABI for sec-k01 (not implemented)

K-Q2 delegates the freeze to this reconciliation. Reserve **CIUKI_SYS_SECURITY
= 101**, following the highest native allocation, NET_INFO=100
(abi.h:761–765). Do not fill F6's 92–95 gap. W32_DISPLAY=0x201 is a suboperation
of call **96**, not a global call 101. Native ABI version 1, old syscall numbers,
existing structure prefixes and call 70's 80-byte proc_record stay unchanged.

Call convention: EAX=101, EBX=user address of ciuki_security_args,
ECX=64, EDX=ESI=EDI=EBP=0. Return EAX=0 on success or negative Linux-i386 errno;
results live in copied output fields. This family is available to admitted
native and Win32 processes; guests cannot supply a host syscall subject.
All wire integers are little-endian fixed-width values, scoped pack(4),
alignment 4; addresses are u32 user virtual addresses, never native pointers.

### Constants and bounds

Every identifier in this table is a literal public constant to be added by
sec-k01, with an independent oracle expectation. Shorthand op names below
refer to the exact CIUKI_SECURITY_ names here.

| Constant | Frozen value / meaning |
| --- | --- |
| CIUKI_SYS_SECURITY | 101 |
| CIUKI_SECURITY_VERSION | 1 (all security records below) |
| CIUKI_UID_SYSTEM | 0 |
| CIUKI_UID_ADMINISTRATOR | 500 |
| CIUKI_UID_GUEST | 501 |
| CIUKI_UID_FIRST_USER | 1000 |
| CIUKI_UID_LAST_USER | 0xfffffffe |
| CIUKI_UID_INVALID | 0xffffffff; never allocated |
| CIUKI_GID_SYSTEM | 0 |
| CIUKI_GID_ADMINISTRATORS | 544 |
| CIUKI_GID_USERS | 545 |
| CIUKI_GID_GUESTS | 546 |
| CIUKI_GID_INVALID | 0xffffffff; never a membership |
| CIUKI_SECURITY_GROUP_MAX | 8; no truncation |
| CIUKI_SECURITY_PROFILE_PATH_MAX | 64 bytes, including NUL |
| CIUKI_SECURITY_SESSION_MAX | 4 retained records, including constructing/closing |
| CIUKI_CRED_INTERACTIVE | 0x00000001 |
| CIUKI_CRED_SAFE_RECOVERY | 0x00000002; only uid 500 in kernel-observed Safe Mode |
| CIUKI_SECURITY_QUERY_SELF | 1 |
| CIUKI_SECURITY_QUERY_PROCESS | 2 |
| CIUKI_SECURITY_QUERY_PEER | 3 |
| CIUKI_SECURITY_SESSION_CREATE | 4 |
| CIUKI_SECURITY_SPAWN_CREDENTIAL | 5 |
| CIUKI_SECURITY_SESSION_END | 6 |
| CIUKI_SECURITY_SESSION_QUERY | 7 |
| CIUKI_SECURITY_SESSION_LAUNCH | 8 |
| CIUKI_SECURITY_LAUNCH_POLL | 9 |
| CIUKI_SECURITY_SECURE_ENTER | 10 |
| CIUKI_SECURITY_SECURE_LEAVE | 11 |
| CIUKI_SECURITY_SERVICE_CONNECT | 12 |
| CIUKI_SECURITY_SERVICE_ACCEPT | 13 |
| CIUKI_SECURITY_SERVICE_CANCEL | 14 |
| CIUKI_SECURITY_LAUNCH_DESKTOP | 1 |
| CIUKI_SECURITY_LAUNCH_W32SRV | 2 |
| CIUKI_SECURITY_LAUNCH_PREPARING | 1 |
| CIUKI_SECURITY_LAUNCH_READY | 2 |
| CIUKI_SECURITY_LAUNCH_RUNNING | 3 |
| CIUKI_SECURITY_LAUNCH_FAILED | 4 |
| CIUKI_SECURITY_SECURE_LOCK | 1 |
| CIUKI_SECURITY_SECURE_SAVER | 2 |
| CIUKI_SECURITY_SECURE_RESUME | 3 |
| CIUKI_SECURITY_SECURE_ATTENTION | 4 |
| CIUKI_SECURITY_CONSOLE_TRUSTED | 1 |
| CIUKI_SECURITY_CONSOLE_USER | 2 |
| CIUKI_SECURITY_ROLE_NONE | 0; ordinary processes |
| CIUKI_SECURITY_ROLE_LOGON | 1 |
| CIUKI_SECURITY_ROLE_LOGON_UI | 2 |
| CIUKI_SECURITY_ROLE_ACCOUNT | 3 |
| CIUKI_SECURITY_ROLE_OWNER | 4 |
| CIUKI_SECURITY_ROLE_REGISTRY | 5 |
| CIUKI_SECURITY_ROLE_DESKTOP | 6 |
| CIUKI_SECURITY_ROLE_W32SRV | 7 |
| CIUKI_SECURITY_ROLE_SETUP | 8; boot-only destination staging |
| CIUKI_SECURITY_SERVICE_AUTH | 1 |
| CIUKI_SECURITY_SERVICE_ACCOUNTS | 2 |
| CIUKI_SECURITY_SERVICE_OWNER | 3 |
| CIUKI_SECURITY_SERVICE_REGISTRY | 4 |
| CIUKI_SECURITY_SERVICE_LOGON | 5 |
| CIUKI_SECURITY_SERVICE_W32SRV | 6; scoped to caller's session |
| CIUKI_SECURITY_SERVICE_BODY_MAX | 8192 |
| CIUKI_SECURITY_SERVICE_PENDING_MAX | 8 total exchanges; one per endpoint |
| CIUKI_SECURITY_SERVICE_FRAGMENT_MAX | 192; 64-byte header + fragment <= existing 256-byte channel payload |
| CIUKI_SECURITY_LAUNCH_TIMEOUT_MS | 5000 |
| CIUKI_SECURITY_SERVICE_TIMEOUT_MS | 5000; reassembly/dispatch admission, not durable commit rollback |
| CIUKI_SECURITY_END_BEGIN | 0x00000001 |
| CIUKI_SECURITY_END_COLLECT | 0x00000002; mutually exclusive with BEGIN |
| CIUKI_SECURITY_SESSION_ACTIVE | 1 |
| CIUKI_SECURITY_SESSION_CLOSING | 2 |
| CIUKI_SECURITY_SESSION_DEAD | 3 |

Session 0 is SYSTEM boot/services only, never an interactive session. Allocate
interactive session ids monotonically 1..0xffffffff per boot, without wrap;
exhaustion EAGAIN. A free internal slot is not session id 0 and is not a public
state. Retired slots may be reclaimed only after final references drain;
SYSTEM can query any issued retired id as DEAD using the allocation high-water
mark without an unbounded tombstone list. UID allocation is persistent, with
0xffffffff reserved for libc's invalid-id convention; no uid is reused.

### Records

Each declaration below occupies one field per line in the eventual abi.h and
SDK copy. These tables, including array extents, are the independent layout
specification. No C enum, bitfield, host long or pointer is a wire field.

ciuki_credential: size 64, alignment 4.

| Declaration | Offset | Bytes |
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

Groups include the primary gid, sorted ascending, unique, count 1..8 and zero
unused slots. Accepted session templates are exactly administrator (uid 500 or
ordinary uid, primary 545, groups [544,545]), Standard (ordinary uid, primary
545, groups [545]), or Guest (uid 501, primary 546, groups [546]). Session id
in a create template is zero and INTERACTIVE is required. SAFE_RECOVERY also
requires uid 500 and actual Safe Mode; ordinary enabled uid 500 logon uses
INTERACTIVE only. Reserved uid ranges, SYSTEM templates, unknown flags and
arbitrary group combinations fail EINVAL. SYSTEM queries return uid/primary
0, group_count=1, groups[0]=0, session=0, flags=0. Administrator membership is
derived by the kernel; no caller-set ADMIN/SYSTEM bit exists.

ciuki_security_profile: size 112, alignment 4. This is a descriptor of an
already committed owner binding, not permission to create/rebind one.

| Declaration | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t uid | 8 | 4 |
| uint32_t flags | 12 | 4 |
| uint32_t volume_id_lo | 16 | 4 |
| uint32_t volume_id_hi | 20 | 4 |
| uint32_t owner_generation_lo | 24 | 4 |
| uint32_t owner_generation_hi | 28 | 4 |
| uint32_t owner_key_lo | 32 | 4 |
| uint32_t owner_key_hi | 36 | 4 |
| uint32_t path_bytes | 40 | 4 |
| uint32_t reserved | 44 | 4 |
| char path[64] | 48 | 64 |

flags/reserved=0; uid matches the template; volume_id identifies the mounted
system volume, not a caller-selected drive letter. Each lo/hi pair encodes one
opaque u64; owner key/generation are nonzero, nonreused committed values.
path_bytes is 1..64 including exactly one terminal NUL, zero padding follows.
The path is the canonical ASCII numeric profile path, including an explicitly
mapped retained G32 numeric root, matching the resolved owner entry exactly.
No relative path, alias, embedded NUL, dot component, arbitrary UTF-8 name or
client-supplied HOME can select a different root. Pending or retired ownership
fails EACCES; generation mismatch EAGAIN; missing/corrupt store EIO. No owner
store I/O occurs on a warm query/read/write path.

ciuki_security_args: size 64, alignment 4.

| Declaration | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t operation | 8 | 4 |
| uint32_t flags | 12 | 4 |
| uint32_t credential_address | 16 | 4 |
| uint32_t spawn_address | 20 | 4 |
| uint32_t session_id | 24 | 4 |
| uint32_t target_pid | 28 | 4 |
| int32_t endpoint_fd | 32 | 4 |
| uint32_t profile_address | 36 | 4 |
| uint32_t output_address | 40 | 4 |
| uint32_t output_bytes | 44 | 4 |
| uint32_t result | 48 | 4 |
| uint32_t state | 52 | 4 |
| uint32_t reserved[2] | 56 | 8 |

ciuki_security_launch: size 96, alignment 4.

| Declaration | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t purpose | 8 | 4 |
| uint32_t flags | 12 | 4 |
| uint32_t session_id | 16 | 4 |
| uint32_t session_generation | 20 | 4 |
| uint32_t launch_id | 24 | 4 |
| uint32_t launch_generation | 28 | 4 |
| uint32_t pid | 32 | 4 |
| uint32_t process_generation | 36 | 4 |
| int32_t control_fd | 40 | 4 |
| int32_t child_ready_fd | 44 | 4 |
| int32_t child_console_fd | 48 | 4 |
| int32_t child_display_fd | 52 | 4 |
| int32_t child_input_fd | 56 | 4 |
| int32_t child_service_fd | 60 | 4 |
| uint32_t service_generation | 64 | 4 |
| uint32_t grant_generation | 68 | 4 |
| uint32_t state | 72 | 4 |
| int32_t error | 76 | 4 |
| uint32_t reserved[4] | 80 | 16 |

ciuki_security_console: size 64, alignment 4.

| Declaration | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t session_id | 8 | 4 |
| uint32_t session_generation | 12 | 4 |
| uint32_t console_generation | 16 | 4 |
| uint32_t reason | 20 | 4 |
| uint32_t grant_generation | 24 | 4 |
| uint32_t restore_cookie_lo | 28 | 4 |
| uint32_t restore_cookie_hi | 32 | 4 |
| uint32_t state | 36 | 4 |
| uint32_t reserved[6] | 40 | 24 |

ciuki_security_link: size 64, alignment 4.

| Declaration | Offset | Bytes |
| --- | ---: | ---: |
| uint32_t version | 0 | 4 |
| uint32_t size | 4 | 4 |
| uint32_t service | 8 | 4 |
| uint32_t flags | 12 | 4 |
| int32_t endpoint_fd | 16 | 4 |
| uint32_t endpoint_generation | 20 | 4 |
| uint32_t server_role | 24 | 4 |
| uint32_t peer_pid | 28 | 4 |
| uint32_t peer_process_generation | 32 | 4 |
| uint32_t peer_role | 36 | 4 |
| uint32_t session_id | 40 | 4 |
| uint32_t service_generation | 44 | 4 |
| uint32_t request_id_lo | 48 | 4 |
| uint32_t request_id_hi | 52 | 4 |
| uint32_t reserved[2] | 56 | 8 |

### Operations, copying and errors

Input version=1, size=64. Fields not expressly listed as inputs below are zero,
including result/state and endpoint_fd on other operations. Nested record
version/size must be exactly 1 and the specified record size (64, 96 or 112). Unknown versions
return EOPNOTSUPP, unknown operations/flags and wrong sizes EINVAL. Before side
effects copy/validate all input records and nested spawn strings/fd lists,
check overflow and pin all output spans writable. Output credential buffers
must be nonzero, exactly 64 bytes and four-byte aligned. Misalignment EINVAL;
inaccessible/wrapping spans EFAULT. Reject overlapping record/output/nested
input spans with EINVAL, rather than relying on copy ordering. On failure,
user outputs are unchanged and no child/session is published. Pinning failure
cannot follow a successful session mutation. On success unused outputs and
all padding are zero, input selector fields are preserved.

| Operation | Nonzero/meaningful inputs | Authority and output |
| --- | --- | --- |
| QUERY_SELF | output_address, output_bytes=64 | Any admitted process; complete self credential in output. |
| QUERY_PROCESS | target_pid (1..INT32_MAX), output_address, output_bytes=64 | Same uid and session for ordinary targets; administrator may query other nonprotected targets; SYSTEM all. Output is the immutable credential only. Protected SYSTEM targets require SYSTEM; public process-list summaries are separate. |
| QUERY_PEER | endpoint_fd (0..127), output_address, output_bytes=64 | Caller must own the live receiving channel endpoint/accepted service link. Snapshot the kernel-retained peer credential, returning endpoint generation in args.result; never a request uid or PID lookup. An ordinary client may query the public credential of its connected SYSTEM broker; this grants no service role. No token for acting as that peer is returned. |
| SESSION_CREATE | credential_address -> 64-byte template, profile_address -> 112-byte descriptor | SYSTEM LOGON role only; one console session, fully checked committed owner binding. Prepare privately, then publish ACTIVE with fresh id in result. No process is started implicitly; UI remains at the trusted gate until desktop ready. |
| SPAWN_CREDENTIAL | session_id, spawn_address -> existing 32-byte ciuki_spawn_args | SYSTEM LOGON role only; live ACTIVE interactive session. Use its immutable credential; no alternate template/profile fields. Publish one native child, result=pid; standard native spawn path/image/ARG_MAX checks apply. |
| SESSION_END | session_id, flags exactly END_BEGIN or END_COLLECT | SYSTEM LOGON role only. BEGIN idempotently changes ACTIVE to CLOSING and schedules bounded teardown; COLLECT polls cleanup without blocking on disk/client. state=CLOSING or DEAD, result=0; repeat on issued retired id returns DEAD. Never permit session 0. |
| SESSION_QUERY | session_id | Same current session, or administrator for retained nonsecret session state, or SYSTEM for all issued ids. state=ACTIVE/CLOSING/DEAD, result=current nonzero session generation (0 only for a freed DEAD record). Unknown id ESRCH; retired ids are queryable only by SYSTEM after their last record is freed. |

A second ACTIVE console session or a still-unfenced CLOSING one returns EBUSY
on CREATE. Four retained slots include unpublished construction and draining
records; exhaustion returns EAGAIN. A failed spawn leaves the session admitted
but launches nothing; LOGON must end it before returning to
LOGON. GROUP/uid fields are trusted only on the SYSTEM LOGON CREATE route,
never on public spawn/exec/Win32 records. No operation changes a live token.

SPAWN_CREDENTIAL copies the current ciuki_spawn_args (size=32, reserved=0,
flags limited to CIUKI_SPAWN_NEW_GROUP) but requires fd_count=0/fd_list=0:
all caller fds including 0–2 are dropped, cwd becomes that profile's Documents,
envp is an explicit copied allowlist, never an inherited SYSTEM environment.
Accept only USER/LOGNAME (the service-supplied validated 1-31-byte account
label), HOME (profile/Documents), USERPROFILE/CIUKI_PROFILE (profile root),
TEMP/TMP/TMPDIR (profile/Local Settings/Temp), PATH (fixed /Programs:/System),
LC_ALL=C and generated TZ. TZ uses the committed timezone rule validated by
the existing timezone parser and CIUKI_TIME_RULE_MAX bound (UTC0 when absent),
not a caller override or copied SYSTEM variable. Reject an invalid supplied rule.
Profile settings are loaded from the committed descriptor under the target
subject; no caller-selected settings file/path. CIUKI_PROFILE_SETTINGS, when
needed, is generated as profile/Settings/desktop.cfg. Missing generated keys
are installed by the kernel/session adapter, including LC_ALL and TZ. Existing
mixer/task descriptor labels are generated only by purpose-specific launch;
they do not enlarge this general spawn allowlist.
Reject duplicates, unknown keys or mismatching path values with EINVAL; add
missing path values from the committed profile. USER/LOGNAME are labels only,
not authority. Never copy SYSTEM environment/stdio by default.
SPAWN_CREDENTIAL never assigns service roles or console grants.
SESSION_LAUNCH below explicitly installs session roles, endpoints and grants. Public native/Win32 child creation thereafter inherits the credential
and allowed same-session fds normally. Account-store/raw-disk/privileged
service descriptors never enter an ordinary child.

Endpoint identity is retained at service CONNECT/ACCEPT and channel creation;
cross-credential handoff is only an explicit SYSTEM-installed broker channel.
Dup/inheritance do not change its peer, and fd close/reuse cannot retarget a
retained request. Closed/revoked links or dead peer sessions return ESRCH, wrong fd/kind
EBADF; querying another process's fd is impossible. Ordinary same-session
links reject a sender whose credential differs from the bound peer. Services
must hold an actual endpoint/subject reference across asynchronous work;
QUERY_PEER's copied answer alone is not a revocation-proof authorization lease.
Sec-k01 provides retained-subject primitives; later brokers use them.

Privilege-family denial is EPERM before target-dependent side effects;
object/profile/target-scope denial EACCES, absent permitted pid/session ESRCH,
invalid fd EBADF, malformed record/flags EINVAL, unsupported version
EOPNOTSUPP, invalid span EFAULT, transient generation/slot/id exhaustion
EAGAIN, active/closing session or mapping barrier EBUSY, allocation ENOMEM,
persistent owner capacity ENOSPC, corrupt/unavailable committed metadata EIO.
No silent success, partial record output, uid truncation or new panic path.
All returned values use the existing errno constants, not a private numbering.
Existing file/worker APIs must additionally permit EACCES after revocation;
EROFS continues to mean actual read-only media. Win32 object denial maps to
ERROR_ACCESS_DENIED (5), privilege-required APIs to ERROR_PRIVILEGE_NOT_HELD
(1314), NT transport to STATUS_ACCESS_DENIED; DOS denial is error 5.

### Session launch and secure console (A1-2)

Operations 8..11 require uid 0 **and ROLE_LOGON** installed by PID 1. Other
SYSTEM services, administrators and public spawns of the same binary receive
EPERM. Use args.session_id plus args.spawn_address for a copied input record,
and args.output_address/output_bytes for a disjoint output record of exactly
its specified size. All other args inputs are zero. The record version/size,
zero flags/reserved, span pinning, unchanged-output-on-error and errno rules
above apply. All new output records must be nonzero/aligned/writable and exactly sized,
with the same overlap rejection as credential outputs. No separate unfrozen
supervisor entry may perform these handoffs.

| Operation | Input record | Result |
| --- | --- | --- |
| SESSION_LAUNCH | launch: version=1, size=96, purpose=DESKTOP or W32SRV, session_id matching args, session_generation from SESSION_QUERY (args.result); all other fields zero | Complete launch record. One child of each purpose per session; duplicates EBUSY. The child is prepared with that session's immutable credential and the fixed sealed-role image (`/Programs/desktop` or `/System/Win32/w32srv`), never an arbitrary spawn path. |
| LAUNCH_POLL | launch: version/size, session id/generation, purpose, launch_id/generation; remaining fields zero | Snapshot PREPARING/READY/RUNNING/FAILED with original pid/process generation, current session/service/grant generations and descriptors; error is zero except FAILED, then negative errno. No wait. A pending handshake returns PREPARING, not an erroneous partial launch. |
| SECURE_ENTER | console: version=1,size=64,session id/generation,reason=LOCK/SAVER/RESUME/ATTENTION; other fields zero | TRUSTED state, fresh nonzero console/grant generation and one-use u64 restore cookie. Idempotent same-reason enter while already trusted returns current binding; it does not remint a cookie. |
| SECURE_LEAVE | console: exact current id/session/console/grant generations, reason and restore cookie; state/reserved zero | USER state after LOGON authorizes the matching owner's unlock. Consumes cookie; stale/replayed generations return EAGAIN, foreign session EACCES. Never mints another session or credential. |

SESSION_CREATE still publishes kernel session ACTIVE, initially generation 1,
while presentation stays trusted. SESSION_QUERY returns current session generation
in args.result (state unchanged); no new session id is needed. Launch admission,
console handoff and CLOSING each advance the nonzero session generation; callers
refresh after a transition. Console, grant, launch, process, service and endpoint
generations never wrap/reuse during a boot; exhaustion EAGAIN before mutation.
On a required secure transition, exhaustion keeps the neutral trusted screen
and allows terminal teardown/cleanup without allocating another generation.
A pending launch is bound to the stable session id and its retained object, not
to a user-supplied current PID. Its later POLL uses the current session generation;
the launch id/generation identifies the original child across other launches.

Before any child becomes runnable, SESSION_LAUNCH reserves all resources and
installs its role, session-scoped service registration, private safe console and
stdio, generated environment and descriptor bindings. Stdio 0..2 refers only to
that session's safe console (input EOF until ordinary ownership); it exposes no
SYSTEM log/history fd. child_console_fd is a duplicate session-log description;
child_ready_fd is the private readiness endpoint; desktop alone receives
child_display_fd/child_input_fd with grants initially **suspended**. w32srv alone
gets child_service_fd (accept-notification descriptor) and the scoped W32SRV role.
No authority fds for account/owner/raw disk/registry backing enter either child.
Unused output fds are -1. control_fd belongs to LOGON; all child_* numbers name
that child only and cannot be used as caller fds. Descriptor values are stable
through readiness; POLL returns -1 after failure/closure, never a recycled slot.
All role/readiness/grant/service descriptions are unconditionally noninheritable
and cannot be sent as channel attachments; clearing CLOEXEC cannot change this.
Ordinary children inherit credentials, never these roles or control endpoints.

Readiness uses the existing channel SEND from the child_ready_fd, exactly one
16-byte little-endian payload: version u32=1, launch_id u32, launch_generation
u32, status i32 (0 ready, negative errno failure); zero attachments. The kernel
checks the retained sending process/generation and assigned purpose. LOGON's
LAUNCH_POLL consumes this handshake; claimed uid/pid/purpose bytes cannot spoof
it. The child receives launch id/generation and fd numbers in kernel-generated
CIUKI_SESSION_*/CIUKI_LAUNCH_* environment labels (decimal values), and grant/
service labels appropriate to its purpose. These labels describe installed fds;
editing them grants nothing. Ready means the service registered and loaded the
correct profile/hive and the desktop has a complete concealed first frame.
Both children must be ready before desktop POLL atomically changes the pair to
RUNNING, transfers the user console and makes the first user frame/input visible.
w32srv readiness alone stays READY. Start w32srv then desktop, refreshing the
session generation between launches. Maximum two launch records per session.

Prepare/copy/resource failure rolls back the unpublished child, role registration,
all endpoints/console/grants and charges, returning unchanged output. Once a child
is published, exit, negative handshake or the 5000 ms monotonic deadline produces
FAILED; stop the entire session pair, revoke descriptors/maps, enter CLOSING and
keep the trusted error gate. LOGON collects teardown before another logon. No
fallback user desktop or SYSTEM restart of a user process. Desktop/w32srv crash
after RUNNING likewise enters trusted presentation and closes that session;
registered role/endpoint generations are retired, never rebound to a new pid.

SECURE_ENTER masks user presentation first, stops user input delivery and drains
or fences every framebuffer mapping, acceleration batch and DOS/Win32 fullscreen
lease before returning success or displaying trusted credentials. A failed fence
keeps a neutral kernel-owned screen and no application input; EBUSY never restores
an exposed desktop. No waiting under the session lock. Application CPU execution,
files and background work continue while locked; only presentation/input is
suspended. Reject screenshot/readback/synthetic unlock routes. Returning from
trusted presentation creates fresh map/lease generations; stale mappings do not
become writable again. LOGON verifies the current owner's password via AUTH
before LEAVE for LOCK/SAVER/RESUME. ATTENTION can return from the security menu
without a password only when it did not request a protected lock; a blank account
gets set-password guidance, never a claim of password protection. Kernel-observed
physical Ctrl+Alt+Delete enters this same trusted transition and notifies LOGON;
ordinary input cannot manufacture that event. Resume arms the transition before
restoring a user frame.

LOGON/LOGON_UI/ACCOUNT crash while locked invalidates pending authentication,
retires the restore cookie and keeps applications running concealed; PID 1
restarts the fixed service role, obtains a fresh cookie through ENTER and demands
fresh authentication. LOGON failure while a session is running also enters the
trusted gate. If a restore cookie was invalidated by service death, ENTER creates a fresh
binding for the replacement registered LOGON, overriding the ordinary idempotent
return rule but never restoring user presentation. If the trusted UI cannot
restart, the neutral error/shutdown screen
stays in control. No stale successful reply can call LEAVE. Session termination
uses END_BEGIN/END_COLLECT and the existing drain contract, not an unlock shortcut.
Sec-k01 implements the operations/role checks and host-testable grant adapter;
sec-k07 integrates **all** real input/display backends before any secure-lock claim.

### Service identities, connection and retained subjects (A1-3)

Boot role images are `/System/Services/logon`, `/System/Services/logon-ui`,
`/System/Services/accounts`, `/System/Services/registry`; OWNER is the kernel
endpoint, SETUP the sealed `/Programs/setup` entry on Setup media. PID 1 installs
LOGON_UI's secure display/input/console descriptions before publication, never
SYSTEM log/history fds. They are unconditionally noninheritable and distinct
from suspended user grants; console transitions activate exactly one domain.
Service restart is fixed-manifest respawn with a fresh role/generation, not
public registration of any path. All listed binaries/configuration/ancestors
are SYSTEM-maintenance-only anchors.

Role is kernel process metadata separate from uid/groups; it is never a writable
credential flag. PID 1 registers one live LOGON, LOGON_UI, ACCOUNT, OWNER and
REGISTRY from the sealed boot manifest (OWNER may be a kernel endpoint), and
boot-only SETUP on installation media. SESSION_LAUNCH alone registers DESKTOP
and W32SRV for an admitted session. Bootstrap/respawn checks protected image,
parents, session and generation before publication; a public copy/spawn/exec gets
ROLE_NONE. No public REGISTER operation, PID/name discovery or administrator role
registration. uid 0 without the required role is denied. Singleton death removes
the registration; only PID 1 can reestablish it with a fresh service generation.

| Service id / registered server | Allowed connecting clients and purpose |
| --- | --- |
| 1 AUTH / ACCOUNT | LOGON_UI only: list eligible logon names, authenticate, unlock and trusted enrollment; no ordinary client's password-to-session primitive. |
| 2 ACCOUNTS / ACCOUNT | Admitted native/Win32 client: filtered lookup, self-password/picture change; administrator-only management checked on each request, not merely CONNECT. LOGON and LOGON_UI can get public state. |
| 3 OWNER / OWNER | ACCOUNT, or boot SETUP only for explicit confirmed repair of an identified installation: full destination batch in main section 3.5. Initial Setup stages data without owner COMMIT; installed ACCOUNT promotes the seed. A uid supplied by a caller is not a subject. |
| 4 REGISTRY / REGISTRY | Admitted native/Win32 clients and registered same-session W32SRV: logical key operations authorized as retained subject, never raw backing paths; machine restart maintenance additionally requires the broker's administrator-checked queue protocol. |
| 5 LOGON / LOGON | ACCOUNT and LOGON_UI only: correlated authentication completion, trusted lock/menu/power events. Only ACCOUNT can report verified authentication; LOGON_UI cannot claim a successful verification. |
| 6 W32SRV / W32SRV | Admitted same-session clients, native or Win32, for existing supported registry/USER compatibility work; another session cannot choose a target session id. |

SERVICE_CONNECT inputs args.spawn_address -> link (version=1,size=64,service,
other fields zero), output_address/bytes -> disjoint 64-byte link; args.session_id
is zero. Kernel selects the registered role and, for service 6, the caller's
immutable session. It returns caller endpoint_fd, endpoint_generation, server_role,
peer pid/process generation/role, caller session_id and service_generation.
No server: ESRCH. Forbidden role/scope: EPERM/EACCES. SERVICE_ACCEPT uses the same
input/output shape and registered service id; only that server role/generation
may accept. Empty backlog EAGAIN. It returns the server fd and actual retained
client metadata, with server_role naming itself. Refuse before allocating if no
live registration, slot or capacity. Endpoint pairs are connected and both sides
bound before publication with one shared endpoint_generation per pair,
noninheritable/nontransferable even to a same-uid child.
Clients cannot choose/inherit the server end or clear its restrictions.
QUERY_PEER remains the credential query; link peer_role is kernel output and
services also use the retained internal subject rather than trusting copied bytes.

SERVICE_CANCEL input link includes service, owned endpoint_fd/generation and
nonzero request_id_lo/hi; others zero, no output buffer. It marks the matching
exchange cancelled: args.result=1 if cancelled before the durable point, 0 if
already irrevocable/completed. Closing a link, session revocation or peer death
performs the same cancellation. No cancelled reply/fd attachment may reach a new
endpoint or session. Cancellation after COMMIT_REQUIRED cannot roll back durable
work; caller learns the result by the transaction STATUS protocol after reconnect.
A partial uncommitted request is discarded. Reconnection always gets a fresh
endpoint generation, never a continuation using a recycled fd number.

Reuse `channel_connect` (`channel.c:307-326`) duplex queues/description accounting,
close/wait/poll cleanup, extending it with retained peer credentials/process and
endpoint generations, role registry and enforced no-inherit/no-transfer bits.
Reuse the accepted-link lifecycle from `win32/service.c:191-242`, replacing its
hard-coded global server assumptions with per-session W32SRV lookup. Existing
Win32 service ids 1..2 stay in their old namespace; do **not** reinterpret them as
these call-101 ids or just widen the old range check. Native adapters connect
through call 101. Win32 service-2 registry connection resolves to its registered
session W32SRV; the new direct broker adapter uses SECURITY service 4. Unchanged
legacy dispatch cannot grant a new role or bypass these checks.

Service framing (private protocol schema 1, not a change to ciuki_channel_message):
one channel message <=256 bytes, no attachments, 64-byte header and <=192-byte
fragment. All fields little-endian, no padding:

| Header offset | Field |
| --- | --- |
| 0 | u32 magic=0x31435343 (bytes CSC1) |
| 4 | u32 version=1 |
| 8 | u32 header_bytes=64 |
| 12 | u32 service_id, matching the connected endpoint |
| 16 | u32 operation, endpoint-specific; unknown returns EOPNOTSUPP |
| 20 | u32 flags: REQUEST=1 or REPLY=2, no other bits |
| 24 | u64 request_id, monotonic nonzero per connection |
| 32 | u32 endpoint_generation, matching the kernel endpoint |
| 36 | u32 fragment_bytes (0..192) |
| 40 | u64 expected_service_data_generation (0 only for pure reads/bootstrap) |
| 48 | i32 status: 0 request/success, negative errno failure |
| 52 | u32 total_body_bytes (0..8192) |
| 56 | u32 fragment_offset |
| 60 | u32 reserved=0 |

Fragments must be contiguous/in order, identical header metadata except offset/
fragment length, exact final total; reject overlaps, gaps, replayed request ids,
unknown flags and mismatched actual message size. One exchange per endpoint,
eight globally, at most 8192 bytes reassembly per exchange; reserve before copy,
EAGAIN for queue pressure, ENOMEM for allocation, EINVAL for malformed framing.
Empty body is one zero-fragment frame. Reply has same request/service/endpoint
identity; failure has no body. No request begins until its complete body validates.
Incomplete reassembly or unaccepted dispatch times out at 5000 ms; an accepted KDF
uses its separate one-worker policy, durable I/O uses storage deadlines. Cancel
via call 101, not forged CANCEL message bytes. Reply buffers/queued work retain
the original subject through final drain; late replies are discarded on revocation.
The per-endpoint service body codecs are in the main contract (accounts/owner)
and the registry contract as amended by main section 6.2 (logical key operations).

Internal retained-subject interface delivered by sec-k01:
`security_subject_acquire_endpoint(process, fd, endpoint_generation, &subject)`
retains peer process **generation**, immutable credential/session, role and link
revocation generation; `security_subject_get/put` balances worker lifetimes;
`security_subject_check(subject, action, scope)` revalidates liveness/scope before
side effects; reply delivery checks the original link/request generation. Values
never come from request uid/pid strings. Queued workers carry the reference into
VFS/registry adapters, cannot substitute their SYSTEM credential and cannot use
a copied QUERY_PEER response as a durable authorization lease. Boot owner/registry
maintenance uses a separately explicit kernel subject. Missing or revoked subject
fails EACCES/ESRCH, releases charges and returns normally without panic.

### Read-only token/SID facade and privilege view

Sec-k01 implements native credential queries, not new W32 token opcodes.
Sec-k06 supplies kernel token-object handles and sec-user-05 supplies the SDK
facade using this same credential/query authorization. Query tokens are
noninheritable, closeable, session-bound handles, never transferable authority
or a credential-setter input. The handle retains its immutable subject and
revocation generation; cross-user/session duplication is refused. A live
process exiting invalidates new process queries; a retained query token remains
readable only while its owning session is live, then is revoked at teardown.

Use local account SID S-1-5-21-A-B-C-uid, SYSTEM S-1-5-18 and built-in groups
S-1-5-32-544/545/546. SYSTEM's native group 0 has no fabricated built-in group
SID: TokenPrimaryGroup maps to S-1-5-18. A/B/C come from protected per-install
entropy-backed first-boot state (M-Q6). No network lookup, impersonation,
assignment, ACL editing, SeDebug/SeTcb or automatic elevation is added.

TokenUser and TokenOwner are the account SID; TokenGroups contains the actual
non-SYSTEM membership with attributes SE_GROUP_MANDATORY(1) |
SE_GROUP_ENABLED_BY_DEFAULT(2) | SE_GROUP_ENABLED(4) = 7. For SYSTEM the group
view contains S-1-5-18 with those attributes. TokenPrimaryGroup is Users/Guests
as above; TokenType=TokenPrimary(1); TokenSessionId is the kernel session.
GetTokenInformation supports only these classes and TokenPrivileges, following
the existing 32-bit Windows SDK layouts and required-buffer-size semantics.
No Windows pointer-bearing TOKEN_* record crosses SYS_SECURITY: the SDK
builds caller-local SID/pointer layouts from the retained kernel query handle.
OpenProcessToken accepts TOKEN_QUERY only; changing rights are denied.
CheckTokenMembership(NULL,...) checks the authenticated current user/groups;
an explicit primary-token handle fails ERROR_BAD_TOKEN_TYPE(1349), as required
by the Microsoft API. DuplicateToken/impersonation remain outside this step.

Freeze the privilege names below as CiukiOS's read-only operation view. LUIDs
are local opaque pairs (LowPart listed, HighPart=0), not a claim of Windows
internal LUID numbers. LookupPrivilegeValue resolves only these names for the
local machine; unknown name ERROR_NO_SUCH_PRIVILEGE(1313), unsupported remote
lookup ERROR_NOT_SUPPORTED(50). TokenPrivileges lists granted entries only,
attributes SE_PRIVILEGE_ENABLED_BY_DEFAULT(1) | SE_PRIVILEGE_ENABLED(2) = 3.

| Name / local LUID LowPart | SYSTEM | Administrator | Standard / Guest |
| --- | --- | --- | --- |
| SeShutdownPrivilege / 1 | Granted | Granted in an interactive session | Granted in an interactive session |
| SeSystemtimePrivilege / 2 | Granted | Granted | Absent |
| SeLoadDriverPrivilege / 3 | Granted | Granted to request supported managed driver changes | Absent |

Privilege queries never skip operation scope, exclusive leases, unsupported
loader checks or SYSTEM-only sealed-file maintenance. Local shutdown still
requires the active session/coordinator; driver privilege enables an approved
management request, never arbitrary kernel code or direct ports. There is no
separate SeTimeZonePrivilege in this XP-inspired subset. AdjustTokenPrivileges,
SetTokenInformation, LogonUser, CreateProcessAsUser and thread-token assignment
fail explicitly using existing typed refusal conventions. Authoritative
checks always occur in the kernel/broker, not in client-built SID memory.

### Implementation obligations

Add every new record and public constant, including syscall/version/uid/gid,
array bounds, credential/end flags, all fourteen operations, role/service ids,
launch purposes/states/timeouts and console reasons/states, to
`tests/host/abi_layout_test.c` with independent literal size/alignment/offset/
array-length/value expectations. Keep one field per line in abi.h and public
SDK copies; synchronize kernel/SDK layout extraction and the i686 JSON oracle.
Run the ABI section of `scripts/test/host_kernel_tests.sh` serially under
`nice -n 19`, preserving its existing sanitizer policy, then
`python3 scripts/gen_api_reference.py`. Extend public Win32 record/constant
oracles when sec-k06/sec-user-05 publish token headers. No such command runs in
this documentation-only task. Future vendoring that includes a .c file also
requires an nm duplicate-external-symbol check against linked libraries.

## Bounded cost, errors and authentication storage

Fixed initial limits are at most 32 static policy prefixes, 32 registered
profile roots on the system volume, 256 creator-owned root entries per volume,
eight groups and four
session records including closing/provisional records. These are reviewable
metadata bounds, not a change to process/VM memory caps. At 64 processes,
one inline 64-byte public-shaped credential would cost at most 4096 bytes;
shared internal records can reduce that. Actual node/handle-label and trie
costs must be measured in sec-k02/sec-k03 rather than claimed free.

Permission checks on read/write/fault/writeback are O(1): subject/session
state, granted operation mask, policy generation, mount/media generation
and node label. Prefix selection is bounded by path components and at most
32 static rules at initial resolution. Ownership is inherited from registered
root anchors (32 profiles plus at most 256 creator records per volume), without scanning the owner
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
locks during derivation. The approved account lockout threshold is
zero: failed guesses never create an indefinite lockout or disable the
recovery account. A delayed retry/backoff is not a persisted account lock.
M-Q5/K-Q6 fix a 1000 ms monotonic retry interval after failure, no persistent
lockout, one active KDF and one pending trusted-gate request; excess work
returns EAGAIN without growing a queue. M-Q2/K-Q7 leave ready-image uid 500
unset and unauthenticatable until an administrator sets an 8-63-byte password.
Custom accounts/Guest also require 8-63 printable ASCII bytes; only the sole
enabled ready-image CiukiUser may have an empty password while its exception
is unretired (A1-1). All lost/corrupt administrator
verifiers require explicit Setup CD repair (main contract section 5.3).

Target acceptance measures derivation on the PIII baseline with the
production SHA-256 implementation and records median/max latency and UI
responsiveness under load. The common target is <=1000 ms per verification,
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

Numbers 92–95 remain unassigned in the F6 reservation. This contract reserves
101 for SYS_SECURITY as specified above; later calls remain unallocated. The
inventory above describes the existing header; call 101 is future sec-k01 work.

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
| INPUT_KEYBOARD_SET | Declared input-owner command, handled by proc/grants.c:353; abi.h:2739. The inspected devctl forwarding condition at proc/syscalls_file.c:385 omits this constant. | own-session keyboard policy; audit/fix the routing in sec-k04 (K-Q8), without claiming it works here. |
| COM_GET_LINE, COM_GET_STATUS, COM_GET_OWNER | Correct COM fd/controller generation; abi.h:73, :75, :77; drivers/com.c:578–588 | own-session port owner, or anyone for a separate nonsecret inventory query. |
| COM_SET_LINE, COM_SET_MODEM | Writable COM owner; abi.h:74, :76; drivers/com.c:588 | own-session bounded line/modem control; never another session/VM/SYSTEM logger's port. |
| COM_SET_MOUSE, including OFF/Microsoft/Logitech/Systems/Wheel protocol | Writable owner; abi.h:78–88; drivers/com.c:588–592 | administrator to change machine input binding; SYSTEM serial-mouse provider performs actual binding. |
| LPT_GET_CONFIG, LPT_GET_STATUS | Port fd; abi.h:165, :167; drivers/lpt.c:565–575 | own-session owner/nonsecret inventory. |
| LPT_SET_CONFIG, LPT_INIT | Writable port owner; abi.h:166, :168; drivers/lpt.c:575–578 | own-session stream/init within admitted mode; administrator persistent machine/VM routing. |
| NET_GET_STATE, NET_QUERY_DRIVER_PACK | Public /dev/net query; abi.h:913–914; execution-abi.md:2725–2733 | anyone; pack inspection cannot activate/install drivers. |
| CIUKI_SETUP_FAT_QUERY, CIUKI_SETUP_FAT_ATTRIB | Setup mode plus noninherited Setup flag; abi.h:1028–1029; proc/setup_file.c:24–26 | SYSTEM Setup only; even read metadata retains the Setup transaction boundary. |
| CIUKI_SETUP_CLOCK_SET | Setup mode plus Setup flag; abi.h:1030; proc/setup_clock.c:10–12 | SYSTEM Setup only; no alternate public clock setter. |
| CIUKI_SETUP_NET_QUERY | Declared at abi.h:1031; no special handler in inspected proc/syscalls_file.c:369–421 | Sec-k04 must audit the route (K-Q8); reject it until fixed, then SYSTEM Setup query only. |
| DISPLAY_GET_MONITOR, DISPLAY_GET_REFRESH | Valid display grant; abi.h:2326–2327; proc/syscalls_file.c:370–384 | own-session grant form; public non-fd inventory can use call 74. |
| MEDIA_GET_STATE, MEDIA_GET_VOLUME, MEDIA_GET_SPACE, MEDIA_GET_BLOCKERS | /dev/media; abi.h:1125–1149, :1189; drivers/media_dev.c:137–266 | anyone nonsecret inventory; blocker rows disclose process name/pid only, never private file paths. |
| MEDIA_SAFE_REMOVE, MEDIA_FLOPPY_LOAD | /dev/media, media/generation/blocker checks; abi.h:1126, :1185; drivers/media_dev.c:230 | own-session local media use; refuse busy/protected system media and retain flush/generation checks. |
| MEDIA_GET_LABEL, MEDIA_SET_LABEL | /dev/media, FAT writable-volume checks; abi.h:1209–1210; drivers/media_dev.c:146 | anyone GET; own-session Users (not Guest) SET on unmanaged removable, administrator SET fixed data volume; SYSTEM sealed/system volume identity. |
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
queues. Main section 6.2 defines separate personal/machine schema-2 journals,
same-uid fresh-session replay, v1 unowned quarantine and broker-authorized machine
work; .DEFAULT never becomes the next user's personal queue. It cannot promote
an HKCU Run entry into machine startup.

Library-only helpers such as clock_getres, sbrk, gettimeofday, pthread
mutex/cond wrappers, ELF/libc startup, POSIX/Win32 priority translations and
SIGRETURN/NtContinue trampolines use the listed calls with the caller's
credential. Internal storage_enable_write/test probe checkpoints, firmware
leases, driver activation and kernel scheduler boosts are SYSTEM internal
actions, not user-authorizable APIs (execution-abi.md:538–565, :1207–1274,
:2785–2813). A diagnostic/test selector cannot be set from an ordinary process
to obtain those internal rights.

## Host acceptance and implementation sequence

The [main contract Implementation plan](user-security-contract.md#implementation-plan)
is the single ordered dependency/parallelism plan for sec-k01..k08 and
sec-user-01..06. It assigns files/areas, exact future host commands, acceptance
cases and lead runtime gates. Do not use the two superseded draft sequences.
New security harnesses must exercise production enforcement paths, including
all selectors in the inventory, with spy backends and fault-injected namespace,
endpoint, ownership, raw-I/O and teardown races. Test both errno and unchanged
external state on denial; preserve partial counts only for admitted issued I/O.
No new panic path, test-only privilege fallback or sanitizer relaxation is
permitted. Run suites serially at nice 19. No host suite runs for this design.

## Decisions

Binding sources: [sec-lead-decisions.md](../directives/sec-lead-decisions.md) and
[Lead amendment 1](../directives/sec-contract.md#lead-amendment-1-2026-10-26-1145-cross-review-round-1).
A1-n identifies the amendment's numbered decision n.
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
| A1-1 | Sole-enabled-account blank CiukiUser; password-or-disable before enabling any other interactive account or blank rename/demotion. Monotonic auto_logon_retired and reboot/transition tests (main 3.1/3.3/4.2, H03/H10). |
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

No new policy question remains open in this reconciliation. Lead cross-review,
implementation, target KDF measurements, runtime security/installer evidence and
hardware qualification remain outstanding. The proposed security QEMU suite
still needs the lead's existing policy authorization before execution; this
document grants none. No runtime evidence is claimed by the design.
