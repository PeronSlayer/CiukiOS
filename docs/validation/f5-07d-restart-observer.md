# f5-07D restart guest observer

The authored fixture exercises the existing f5-07B implementation. Sources
inspected on 2026-10-21 before implementation:

- Microsoft Platform SDK, [Command-String Interface](https://techshelps.github.io/Win32/html/m40p2g.htm): concatenated bracketed commands and PROGMAN conversation names.
- Microsoft [KB Q138800](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/138/800.HTM): ReplaceItem followed by AddItem, selected group semantics.
- Microsoft [Using DDE](https://learn.microsoft.com/en-us/windows/win32/dataxchg/using-dynamic-data-exchange) and [XTYP_EXECUTE](https://learn.microsoft.com/en-us/windows/win32/dataxchg/xtyp-execute): synchronous initiate, posted execute/ACK, CF_TEXT requests and handle ownership.
- Microsoft [WM_QUERYENDSESSION](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-queryendsession) and [ExitWindowsEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex): asynchronous session request, veto, ENDSESSION_LOGOFF. The test uses the existing virtual Windows session; modern NT privileges and physical shutdown policy are not imported.
- NSIS upstream [`RenameViaWininit`](https://github.com/NSIS-Dev/nsis/blob/master/Source/exehead/util.c#L527): destination=source records in [Rename], with NUL for deletion; this corroborates the existing service operands. No upstream code was copied.
- Microsoft [KB Q137367](https://ftp.zx.net.nz/pub/archive/ftp.microsoft.com/MISC/KB/en-us/137/367.HTM): RunServicesOnce and deletion before execution for RunOnce/RunServicesOnce. Queue order follows the already approved f5-07B contract, rather than inferring a global ordering guarantee from modern Windows documentation.
- Owner's Windows 98 Resource Kit, read locally without copying private material: chapter 10, `wrkc10_5.htm` (RunServicesOnce before logon, one execution); chapter 27, `wrkc27_2.htm` (WUCheckShortcut on next startup); Appendix D, `wrkappd_3.htm` (ordered RunOnce batch entries and WININIT.INI DIRNUL). These chapters corroborate f5-07B's existing decisions. Q102575's archive returned HTTP 403; the test checks the existing server's CRLF Groups response.

Implementation decisions: reuse w32srv's existing restart journal, public USER
DDE paths and public ADVAPI registry paths. Persist accumulated fixture results
under C:\WINDOWS\TEMP between PE instances. The reserved result page is
0x10050000, checked after production loader startup by the host loader model.
An actual service interruption occurs while the recovery RunOnce child is
parked after its durable order marker, before the next queue entry. The kernel
requires the journal, a different service PID and the resumed ordered effects.
No build variants or new panic paths are introduced. Guest execution is the
lead's responsibility; host results are not guest qualification.

## Observer boundaries

Four main PE instances run in sequence: DDE plus veto/accept logoff; normal
restart preparation; normal-restart verification and recovery preparation;
recovery verification. Five short RunOnce children append `S`, `M`, `U`, `R`,
`T` to one durable file. Each first proves its own registry value was consumed.
The kernel holds them at the result page long enough to inspect their exact
ancestry, queue journal and file effects. It retains both the authored child's
and runonce.exe's kernel process identities until their normal exits are proven.
The `R` child is held across an actual w32srv termination and replacement.
The desktop independently queries all five consumed values over its copied
registry endpoint before launching the final main instance.

DDE checkpoints hold both original and replaced links while the kernel reads
the actual files. Both client styles request Groups. The second notified window
vetoes logoff, so the test does not depend on the current top-level z-order.
The next request accepts both windows. The six suite predicates are unchanged,
including `desktop_resumed`, which also requires an acknowledged desktop/native
survivor gate snapshot after the last restart and is present in addition to the
five cases enumerated in the directive.

The gate opens C: before starting the desktop service and waits for a successful
registry open/close before each PE launch. `GATE_W32_FIXTURE` is absent in this
worktree, so this uses the authorized fallback `GATE_RESTART`. Merge integration
must unify it with f5-21b's reusable gate. Existing gate command values are
preserved. Gate-only reaping retains intermediate PE exit records until the
kernel sees them and allows the service supervisor to replace w32srv.

The existing restart liveness check used kill(pid,0), whose EPERM result across
process groups also covers exited participants on this kernel. It now consults
the process directory and treats only LIVE/STOPPING as running. This avoids an
indefinite restart wait for the fixture's separate process group.

## Host evidence (2026-10-21)

The selector/menu/model checks and loader size bound pass (12 tests). The
focused native desktop lifecycle check passes (one test, including LIVE,
STOPPING, ZOMBIE and absent-PID liveness). The six unchanged suite predicates
accept the shared C publisher's completed host output; twelve mutations
(missing checked/error evidence for every predicate) are rejected. C negative
cases also exercise unchecked/failed fixture results, missing kernel facts,
corrupt state and duplicate RunOnce invocation.

The shell's five sanitized bodies, including the new four-instance fixture,
reach their PASS markers, but all five tests exit unsuccessfully because this
sandbox prevents LeakSanitizer's ptrace scan. Leak detection remains enabled.
`make CAP= apps` was attempted once and stops at the SDK compiler pin: installed
LLVM 23.1.3 differs from the qualified 23.1.1 executable. The first production-loader
result-page attempt stops at the same qualification check. Neither check was
bypassed. Later, concurrent edits (not made by this implementation session)
updated the SDK, Win32 and Wine tool pins to 23.1.3. With those working-tree pins,
the focused production-loader test passes, including real builtin import
resolution and proof that 0x10050000 is free after startup and can be committed,
written and released. Targeted kernel, desktop-fixture and loader-harness syntax
checks also pass. The apps target was not retried, preserving its one-attempt
limit. The lead owns qualification of the concurrent pins and the remaining apps
and sanitizer validation. Exact commands and logs are in
`build/codex/f5-07d-report.md`.
