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

## Implemented boundaries

The seven SECURITY operations use the frozen records unchanged. Subjects are
assigned in `proc_prepare`, retained by channels and released after process
collection. Native and Win32 child/thread publication rechecks session liveness.
Channel-pair preparation reserves its first child binding privately; publication
commits it, abort preserves the old peer, and subsequent duplication keeps that
binding. Closing/reusing an fd cannot retarget a retained request.

Cross-credential spawn accepts no descriptor list or inherited stdio, resolves
a new cwd from the boot namespace root, and rebuilds only the approved
environment entries. Its required k03 `security_exec_open(child,path,out)`
adapter must authorize and freeze the resolved executable together; a separate
pathname precheck would permit a replacement race. An absent adapter returns
EIO. The required k02 `security_owner_verify(profile)` adapter likewise returns
EIO when absent. This stage supplies neither a synthetic committed owner store
nor a fallback that opens an executable under the SYSTEM caller.

The interim bootstrap proposal uses a kernel-created uid 1000 administrator
session for the desktop and retains session 0/SYSTEM for Setup and service/test
boots. The desktop receives newly constructed stdio and grants; it receives no
SYSTEM descriptor table or cwd. Its existing fixed HOME/TMPDIR defaults remain
until sec-user-03 installs the real profile/logon flow. An unpublished failed
desktop releases its provisional bootstrap session. The fixed internal account
service entry is `/System/Security/accounts`; normal boot attempts it once,
missing-image ENOENT is tolerated for this transition, and a second live
instance is refused. Sec-user-01 supplies its image. Public spawning that pathname inherits the caller's
identity. These bootstrap choices require lead confirmation: the referenced
contracts did not actually contain the interim rule mentioned by the directive.

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
