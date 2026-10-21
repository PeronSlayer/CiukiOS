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
0x100d0000 after Amendment 2, checked after production loader startup by the host loader model.
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

The probe opens C: before starting the desktop service and waits for a successful
registry open/close before each PE launch. Amendment 2 replaces the original
fallback with `GATE_W32_FIXTURE` and the allowlisted `restart-exercise` basename.
The shared desktop gate owns phased relaunch, automatic confirmation only for
the fixture's PID, registry-consumption reports and service-only reaping.
Intermediate PE exits are reported to the kernel before the coordinator reaps
them. The production supervisor replaces w32srv after an interruption.

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

## Amendment 1 host harness correction (2026-10-21)

The lead's gate log links the unselected restart controller from the shared
F5 registration section into the native desktop harness, leaving its VFS and
Win32 object references unresolved. [Clang's attribute documentation](https://clang.llvm.org/docs/AttributeReference.html#used)
confirms that `used` forces emission of otherwise unused definitions. The host
harness calls controllers directly, so its include now uses ordinary unused
registration declarations, with the production macro restored afterwards.
Production linker registration and controller execution are unchanged.

The evidence ownership list now explicitly includes the registered
`f5_restart_observer.inc` controller. Its records remain inside
`probe_w32_restart`; the list grants no general permission to runtime helpers.
The restart suite retains `integration_pending` as an authored, unrun guest
qualification note; the kernel-core test now expects its registered selector.
Amendment commands, results and sandbox limits are recorded in
`build/codex/f5-07d-report.md`.

## Amendment 2 merge integration (2026-10-21)

Rechecked the primary Microsoft SDK/DDE/session documentation, Q138800,
Q137367 and upstream NSIS `RenameViaWininit` linked above before changing the
gate. Their command, veto, asynchronous restart and deletion-before-execution
semantics remain the basis of the existing fixture. No new Windows behavior
is inferred. The private Resource Kit extraction is absent in this worktree;
its historical research record above is retained, not claimed as a new read.

The merged f5-21b core pages occupy `0x10030000` through `0x100c0000`.
The restart protocol now uses `0x100d0000`; Microsoft's
[VirtualAlloc reference](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc)
confirms zero-initialized reserve/commit semantics. The gate host harness asserts
that it differs from all ten core pages, and the production-loader host test
checks actual imports, free reservation, zeroed read/write commit and release.
The fallback command and desktop hooks are removed; phased restart automation
now runs inside the shared gate without extending the ten-core-fixture schema.
The kernel, loader and model tables retain all merged probes and register
restart exactly once. NSIS remains the only unregistered f5-core selector;
the separate authored title selectors retain their existing owners.
`integration_pending` records authored, unrun guest qualification for restart
without suggesting that its observer is still absent.

Amendment 1's narrow evidence ownership entry and direct-call host registration
seam are preserved. Exact Amendment 2 commands, outcomes and remaining lead
validation are recorded in `build/codex/f5-07d-report.md`.

Amendment 2 host evidence: kernel-core (5), probe registry (7), production
kernel-loader (15, including the 137-image corpus), runner selectors (6),
desktop gate (11), boot modes (13, including the fixed loader size) and the
single `make CAP= apps` invocation pass. Shell (5) and the extended shared-gate
harness reach their C PASS markers but fail their test commands at this
sandbox's LeakSanitizer ptrace restriction. The kernel script passes evidence
ownership and its early checks, then stops at the same restriction in
`record_runtime_test`; it is not a green full kernel suite. No sanitizer policy
was overridden and no guest qualification is claimed.
