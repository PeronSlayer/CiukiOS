# Credential foundation host validation

Research checked on 2026-10-26 after running `date -Is`.

Microsoft's [access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
associate user, groups and logon identity with a process. Its
[well-known SIDs](https://learn.microsoft.com/en-us/windows/win32/secauthz/well-known-sids)
distinguish SYSTEM, account RIDs and builtin group RIDs. The
[Open Group getuid specification](https://pubs.opengroup.org/onlinepubs/009695399/functions/getuid.html)
returns the calling process identity. The getgroups page returned HTTP 403;
the frozen CiukiOS group-list contract supplies the bounded subset semantics.
The contract's [Open Group access reference](https://pubs.opengroup.org/onlinepubs/9699919799/functions/access.html)
also returned HTTP 403; its atomic-operation requirement remains binding.
The upstream [Linux man-pages access(2)](https://man7.org/linux/man-pages/man2/access.2.html)
independently documents the replacement race between a permission precheck and
the actual open.
The owner's read-only Resource Kit `wrkc07_3.htm:10-54` and
`wrkc18_2.htm:10-24` distinguish profile storage from local/network logon.
No private book text is copied here.

Implementation decision: retain immutable kernel subjects, independent of PID
and descriptor reuse; derive administrator membership from groups; keep uid 0
exclusive to boot services. Threads share their process subject. Session and
IPC references prevent slot reuse until final release. Public queries never
trust a request's asserted uid. Unsupported identity setters return EPERM.
No NTVDM or Windows 98 Cancel-to-default authentication model is introduced.

The baseline process preparation has no credential, and channel sender_pid
alone cannot support asynchronous subject retention. These are the k01
integration points. Owner binding verification is an explicit fail-closed k02
adapter; it cannot accept a descriptor merely because its fields look valid.

Exact commands, results, bootstrap decisions and remaining integration gates
are recorded in `build/codex/sec-k01-report.md`.

## Lead amendment 1 research and implementation decision

Rechecked 2026-10-26 after `date -Is`. Microsoft's
[desktops](https://learn.microsoft.com/en-us/windows/win32/winstation/desktops)
describes one active input desktop and restricted Winlogon presentation;
[access tokens](https://learn.microsoft.com/en-us/windows/win32/secauthz/access-tokens)
associates execution and work with the process identity;
[cancellation](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
distinguishes requesting cancellation from observing a cancelled operation.
These support the frozen contract's separate presentation/CPU state, retained
subjects and pre-dequeue cancellation boundary. Exact epochs, fixed roles,
framing and transport result values are CiukiOS choices in the reconciled
contract, rather than Windows API compatibility claims.

The partial amendment already covered operations 8-14, but inspection found
missing fixed-role respawn, reused child-fd reporting, and a failed-fence
admission guard. Complete these inside k01; preserve the interim bootstrap
only before the trusted LOGON role is first reserved. Carry target-subject
profile metadata into generated LC_ALL/TZ/settings rather than consulting
SYSTEM's environment. PROFILE=8 is an ACCOUNTS service query supplied by
sec-user-01 and consumed by sec-user-05; it adds no seventh public ABI record
or fifteenth syscall operation here. K02/K03 supply the committed owner and
atomic child-subject executable adapters, and k07 qualifies real device fences.

## Implemented boundaries

The fourteen SECURITY operations use the six frozen records. Subjects are
assigned in `proc_prepare`, retained by channels and released after process
collection. Native and Win32 child/thread publication rechecks session liveness.
Ordinary channel inheritance and duplication preserve the established peer and
generation. Initial handoff is explicit. Common close, dup2 replacement and
teardown detach before revoking the last owner fd. Retained work cannot retarget
through fd reuse. Readiness has a kernel-owned coordinator endpoint and may be
recovered only by a replacement registered LOGON; it never transfers to a child.

Purpose-specific launch prepares private safe console/stdio, suspended grants,
role/service metadata, generated environment and a private ready endpoint before
publication. POLL reports the paired handshake or its 5000 ms failure; it cannot
release a superseding secure gate. Closed child descriptors report -1 even when
the slot is reused. Service links enforce retained role/session routes, no
inheritance/transfer, one exchange per link and eight total. Body reassembly
uses at most two pages per exchange, including one-byte fragmentation; only a
complete bounded body becomes readable. CANCEL proves cancellation only before
the first successful server copy. Closing/revocation destroys buffered replies.

Console transitions serialize with interrupts masked and nonblocking backend
conceal/activate callbacks. A fence still draining returns EBUSY under neutral
presentation. The authentication-required bit, console/authentication epochs
and one-use cookie survive LOGON/UI/ACCOUNT replacement; applications keep their
credential and execution. Failed containment holds CLOSING until a retry fences
the device. Backend qualification and physical-input routing remain k07 work.

Cross-credential spawn accepts no descriptor list or inherited stdio, resolves
a new cwd from the boot namespace root, and rebuilds only the approved
environment entries. Its required k03 `security_exec_open(child,path,out)`
adapter must authorize and freeze the resolved executable together; a separate
pathname precheck would permit a replacement race. An absent adapter returns
EIO. The required k02 `security_owner_verify(profile)` adapter likewise returns
EIO when absent. This stage supplies neither a synthetic committed owner store
nor a fallback that opens an executable under the SYSTEM caller.

The approved interim bootstrap uses a kernel-created uid 1000 administrator
session for the desktop and retains session 0/SYSTEM for Setup and service/test
boots. The desktop receives newly constructed stdio and grants; it receives no
SYSTEM descriptor table or cwd. Its existing fixed HOME/TMPDIR defaults remain
until sec-user-03 installs the real profile/logon flow. An unpublished failed
desktop releases its provisional bootstrap session. The fixed internal account
service entry is now `/System/Services/accounts`, as frozen by amendment 1.
The manifest also fixes LOGON, LOGON_UI and REGISTRY; OWNER is PID 1's kernel
endpoint and SETUP is the sealed installation entry. Missing LOGON ENOENT keeps
the approved interim boot before any LOGON reservation. After the first trusted
LOGON reservation that choice is irreversible for the boot: a service crash
cannot reopen legacy registration or a fallback desktop. PID 1 respawns only
fixed roles it previously started, with fresh nonreused generations. Public
spawns and descendants retain their credential and receive ROLE_NONE.

## Evidence limits

H01 reaches all assertion summaries and zero-resource ledgers, including all
input-copy and allocation failures in credential spawn. The native lifecycle,
signal, desktop and file regressions also reach their final PASS records.
LeakSanitizer then fails because this sandbox does not permit its thread
inspection. These command exits are failures, not green sanitizer evidence;
leak detection remains enabled. Win32's existing harnesses retain their own
unchanged sanitizer settings. Its kernel-core suite and 137-image loader corpus
pass. The complete loader run has one unrelated missing qualified
`advpack.dll` fixture error. The report records exact exits and commands.

No image, guest execution, hardware qualification or complete security policy
is claimed by this host-only foundation.
