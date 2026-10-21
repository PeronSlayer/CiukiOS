# F5-15A2 installer qualification

Research date: 2026-10-21. Host evidence only. Exact commands, outputs and
remaining execution limits are in `build/codex/f5-15a2-report.md`.

## Amendment 4 research and decisions

The lead's stage-4 failure occurs at the first restart journal checkpoint,
before uninstall (stage 6). `flow_driver.py` calls it immediately after
`ws_restart_begin`, before WININIT processing and `ws_restart_next`.
The production restart service deletes RunOnce durably at command handoff,
consistent with Microsoft's [RunOnce documentation](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys).
Therefore require the exact RunOnce command at stages 3 **and 4**, and its
absence at stage 5, after the real child has completed. Keep every Uninstall
value, missing-key check and final deletion expectation. The native desktop's
asynchronous journal checkpoint does not query registry state during handoff.

The lead's INF record reaches `W32_PROCESS` once, then `TERMINATE 11`, with
`kernel_enosys=0`. Exit 11 identifies the fixture's `RunSetupCommandA` call.
Microsoft's [RunSetupCommand contract](https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/aa768010(v=vs.85))
permits the INF/quiet flags already used by this caller. The pinned Wine 10.0
`dlls/advpack/install.c:152–175,850–881` runs the INF post-setup command with
`CreateProcessW(CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP)` and
waits for completion. Both the H1 backend and production process endpoint
reject these bits, returning Win32 error 87 inside a successful transport.
This stops the call before executable search can retry the path with spaces.
The source diagnosis explains the single process request; a complete PE
rerun remains required because this sandbox denies the i386 preflight.

Accept these two documented [process creation flags](https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags)
in the production endpoint and H1 backend. A new group uses the child's PID;
`CREATE_NEW_CONSOLE` retains its documented precedence over the group flag.
The current core initializes error mode afresh in each child, so default
error mode needs no extra state. H1 uses a new host session for the requested
group and still creates a confined adapter child with a real waitable handle.
Keep unsupported flags rejected. Do not alter the authored INF, CAB, NSIS
binary or their semantic manifests. Native regressions exercise these exact
flags, real child completion, group identity, stage-4 RunOnce retention and
stage-5 consumption; they are not compiled-installer acceptance evidence.

The installer controller is already registered with `CIUKI_F5_PROBE` in
`f2_probes_desktop.c`, and its evidence is produced in `installer_probe.inc`.
Register that include in the explicit producer ownership list, matching
`f5_restart_observer.inc`. No service is allowed to emit installer evidence,
and the evidence-scope scanner remains enabled for every other include.

Amendment 4 host verification: 27 installer checks (22 pass, three i386
preflight skips and two LeakSanitizer skips); 30 desktop/selector/boot checks
pass; 12 kernel-core/registration checks pass, including 63,792 native core
assertions. Apps and cached upper builds pass. The changed process endpoint
compiles with canonical i686 flags. The kernel host script passes the evidence
scope check, then stops at the unmodified `record_runtime_test` because
LeakSanitizer cannot inspect threads. A separate observer compile confirms the
lead-noted `restart_link` frame failure (4120 > 4096 bytes); it is left for the
lead's main merge as Amendment 4 requires. The current i386 preflight returns
SIGSYS (-31); successful compiled NSIS, INF/self-extractor or FDI execution is
not claimed. Exact commands, logs and source citations are in the report.

## Amendment 3 research and integration

The lead's new `infchain.exe` record ends with `TERMINATE 22`, after file
copy and registry requests complete without ENOSYS. The authored caller
assigns 22 only when its post-install INI lookup fails. Its three section
flags were `0x10, 4, 1`; the pinned Wine 10.0
[SetupAPI header](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/include/setupapi.h)
defines `SPINST_INIFILES=2` and `SPINST_LOGCONFIG=1`. The
[installation engine](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/setupapi/install.c)
tests `SPINST_INIFILES` before applying `UpdateInis`. Correct the authored
caller to use 2, retaining the callback/context fix and all INF, registry,
file and INI expectations. Only `infchain.exe`'s expected artifact identity
changes, from `d7c5ecc7…` to `9990e3b4…`. This explains the observed exit
code; successful reused-PE execution still requires the lead's rerun.

For the absent shortcuts, the pinned NSIS 2.51 `exehead/exec.c:1040–1066`
calls `CoCreateInstance` before `IShellLink`/`IPersistFile::Save`, as its
[CreateShortCut documentation](https://nsis.sourceforge.io/Reference/CreateShortCut)
describes. Pinned Wine 10.0 `combase/apartment.c:131` loads that class server
using `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`. CiukiOS's
`kernel32/modules.c` rejected flag 8, so this COM activation could never
reach the CiukiOS link writer. The production facade and loader now accept
that flag. For a path, import lookup substitutes the top DLL's folder for
the executable's folder; the context is restored before initialization and
after a failed load. Basename loads use ordinary search. Protected builtins
retain precedence. This follows Microsoft's
[LoadLibraryEx contract](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexa)
and [alternate search order](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order),
including restoration before DLL initialization. It does not enable the
modern `LOAD_LIBRARY_SEARCH_*` flags or NT security policy.
The complete shortcut flow remains unqualified in this sandbox; this is a
source-proven rejection on the actual COM path, not a captured successful
NSIS run. The manifest and `.nsi` script are unchanged.

All ten merge-conflicted files retain both the installer work and the newer
main registrations/observers. The installer uses `GATE_W32_FIXTURE` with the
allowlisted `ciuki-installer-fixture` name; there is no private installer
gate command. Its native coordinator publishes a checkpoint page at
`0x100f0000`, mapped into the desktop by the selected kernel probe with
`ua_map_at`, which refuses overlap. This is deliberately outside native
`mmap`'s `0x20000000` arena without changing that ABI. The unmodified NSIS
engine does not own this page. The authenticated kernel hook compares the
copied checkpoint fields with the report, then reads file/link/journal facts
independently and retains the production read-only w32srv checks. Desktop
teardown releases the mapping. The new address differs from the core and
restart result pages. Single registration and removal from both pending
lists are verified by the existing table checks.

The desktop gate harness explicitly refuses installer VFS operations: its
native spawn replay does not mount an installer filesystem. The installer
suite continues to compile the actual kernel observer with its fixture-tree
VFS transport. Native controller-page and shared-gate tests exercise the
coordinator protocol separately from PE execution evidence. Leak detection
settings and the i386 confinement preflight are preserved.

Primary references checked before implementation:

- [NSIS compiler and installer command line](https://nsis.sourceforge.io/Docs/Chapter3.html):
  use the pinned compiler, explicit `NSISDIR`, no machine configuration;
  `/S` is case sensitive, and `_?=` must be the last uninstaller argument.
- [NSIS uninstaller return codes](https://nsis.sourceforge.io/Docs/AppendixD.html):
  wait for the temporary uninstaller copy with `_?=`, rather than treating
  the original launcher's exit as completion. The Uninstall entry is written
  by the fixture, never synthesized by CiukiOS.
- [NSIS instructions](https://nsis.sourceforge.io/Docs/Chapter4.html):
  the authored cancellation path must explicitly roll back copies and
  registry values. NSIS does not supply a transaction for arbitrary script
  side effects. Separate install/cancel/restart/uninstall manifests are required.
- Wine 10.0 [advpack](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/advpack/install.c)
  and [setupapi](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/setupapi/install.c):
  retain the reused engines. `advpack` routes files, registry and INI work
  through `SetupInstallFromInfSectionW`, then handles advanced commands.
  No independent CiukiOS INF interpreter is substituted for these engines.
- Microsoft [SetupInstallFromInfSection](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfrominfsectiona),
  [DestinationDirs](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-destinationdirs-section),
  [SourceDisksFiles](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-sourcedisksfiles-section),
  [AddReg](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive):
  exercise the flags independently, directory IDs, typed registry data and
  source media. The current scope is application setup, not device installation.
- Microsoft [FDICopy](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fdicopy)
  and Wine 10.0 [FDI](https://github.com/wine-mirror/wine/blob/wine-10.0/dlls/cabinet/fdi.c):
  callbacks own output names and handles. The authored writer produces one
  MSZIP folder with 32 KiB blocks, `CK` signatures and CFDATA checksums.
  Extraction tests compare bytes/hashes independently of the writer.
- [MoveFileEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa)
  describes the current NT API; it is not a Windows 98 restart specification.
  The binding f5-15A amendment requires error 120 on CiukiOS, explicit 9x
  `WININIT.INI` writes by installers and f5-07B replay. The WIP change is
  retained on that authority. No NT Session Manager queue is introduced.

The owner's licensed Resource Kit extraction and predecessor reports are
absent from this worktree. No private material was copied or downloaded.
Existing checked-in f5-15A research records the original Resource Kit review.

The NSIS host recipe uses only the two SHA-256/size pins in `config/nsis.json`.
The 2015 recipe's i386 ELF is killed with SIGSYS before `-VERSION` here.
The native host adaptation preserves 32-bit Windows LONGs and export RVAs,
widens host resource-editor pointer bookkeeping and adds missing integer/POSIX
declarations, recorded exactly by `scripts/build_nsis.py` in
`build/tools/nsis/build.json`. Windows stubs and compression sources remain
the pinned release bytes. `build.json` also records the compiler identity.

The actual NSIS PE's eager import check found `advapi32!SetFileSecurityA`
absent. The existing `security.c` already supplies `SetFileSecurityW` and
both `GetFileSecurity` forms as typed W21 failures (the f5-20A/20B
`w21-nt-security` decision). The ANSI setter now uses the identical refusal:
FALSE, error 120, no descriptor/file mutation. Microsoft's
[SetFileSecurity ABI](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setfilesecuritya)
and the pinned NSIS stub's actual imports were checked; this closes binding,
and does not claim FAT ACL enforcement or installer execution.

Implementation and evidence boundary:

- The canonical apps producer builds/stages the pinned installer, authored
  child, cabinet (INF, child and payload), INF chain, self-extractor and NSIS
  notice. Artifact hashes and the independent semantic expectations are in
  `tests/win32/fixtures/nsis/expected.json`. The compiler is run twice with
  normalized input mtimes; warnings or differing outputs fail the build.
- The runtime desktop driver selects partial cancellation, normal UI install
  (Return through the actual desktop input path), restart request and a waited
  temporary uninstaller copy with `_?=`. It queries all five Uninstall values
  and RunOnce through w32srv; an empty stale Uninstall key also fails.
- The registered kernel probe reads payload/link bytes and presence through
  its own VFS table at synchronous checkpoints, observes both restart journals
  and delayed cleanup, and publishes the four required predicates only from
  those independent observations. Incomplete/invalid sequences fail closed.
- Host checks cover the registry observer against the production service,
  profile refusals, actual PE import closure, deterministic builds, independent
  MSZIP decoding/checksums, and observer/parser negative vectors. The full
  installer runtime and the real probe dry-run are **not qualified**: the
  confined i386 adapter dies with SIGSYS before PE startup. LeakSanitizer's
  ptrace-dependent inspection is unavailable, with leak detection enabled.

At WIP commit `68a7a7fc`, the baseline H1 adapter additionally lacked file,
registry and child-process transports. Amendment 1 below adds those services
and the four-flow driver. Execution evidence remains separate from their
implementation: the four self-deleting patterns and the actual compiled
installer probe are not fully qualified. This delivery must not be treated
as closing all f5-15A2 acceptance criteria.

## Amendment 1 implementation plan and sources

The H1 installer extension transports copied records over an inherited socket
to a fixture-only host service. Guest paths resolve under one temporary tree;
the service never invokes a shell. Registry exchanges use `wr_dispatch` from
the production w32srv model, and restart uses the production `ws_restart_*`
engine, including its journal and deletion-before-RunOnce handoff. This keeps
installer results separate from the observer's read-only manifests. The
existing H1 seccomp preflight remains mandatory before any PE execution.

Microsoft's [CreateProcessA](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessa)
and [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
specify separate process/thread handles and a wait that becomes signaled only
after process exit. The host service therefore launches an actual child adapter
process and polls/waits its exit; it must not manufacture successful child
completion. The [NSIS command-line specification](https://nsis.sourceforge.io/Docs/Chapter3.html)
requires `_?=` last for the waited temporary uninstaller. The
[NSIS scripting reference](https://nsis.sourceforge.io/Docs/Chapter4.html)
leaves rollback to the script. The host driver checks the authored cancellation
after partial extraction, as well as install, restart and uninstall separately.
The [Wine 10.0 setupapi implementation](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/setupapi/install.c)
and [Microsoft SetupInstallFromInfSectionA](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfrominfsectiona)
confirm the independent registry, file and INI flags: retain that engine and
check its effects, rather than implementing a second INF parser in the adapter.

The pinned Wine `dlls/setupapi/parser.c:953` opens INF content through
`CreateFileMappingW(PAGE_READONLY)` and `MapViewOfFile(FILE_MAP_READ)`, closing
the section before parsing the view. The installer extension supplies that
read-only, unnamed mapping path with a separately owned view, so closing the
section cannot invalidate the parser's bytes. Bounds and protection follow
[CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga)
and [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile).
This limited adapter path is not evidence for writable/shared section coherence.

Amendment 1 host checks cover copied-record framing, actual fixture files,
file-sharing errors, temp paths, read-only/full-disk errors, registry transport,
named mutex lifetime, and a real native transport child whose exit status is
23 (not a synthesized successful setup). A UBSan executable exercises the
production registry/restart bridge, including journal visibility, WININIT
deletion and backup, and RunOnce handoff. The production probe's actual VFS
observer is compiled into that bridge behind `CIUKI_INSTALLER_OBSERVER_ONLY`;
corrupt fixture bytes and incomplete sequences cannot publish success.

The compiled-PE driver checks the CAB/INF and self-extractor manifests, then
NSIS cancel, install, restart and temporary-copy uninstall. It calls the actual
file/registry observer for all eight checkpoints and the real four-predicate
publisher. Its NSIS path currently uses `/S`; the normal visible-page path
still needs H1 USER transport/thread support. The guest desktop's visible-page
driver remains in the WIP, without execution qualification here. The preserved
i386 preflight reports SIGSYS before any PE startup, so neither the new driver
nor its read-only mapping endpoint has runtime evidence in this sandbox.

## Amendment 2: lead failures and scoped corrections

The lead's `build/lead/installers.log` reports a missing desktop shortcut,
FDI failure with eight unsupported kernel records, and an INF access violation.
The sandbox still terminates the i386 adapter before PE startup. These three
runtime acceptance checks therefore remain open; native unit results do not
supersede the lead's failures.

The pinned NSIS 2.51 `Source/exehead/util.c:669–738` explicitly chooses
`SHGetSpecialFolderLocation` / `SHGetPathFromIDList` on Windows 95/98 for
Desktop and Programs, rather than `SHGetFolderPath`. Those exports previously
stayed in the reused Wine shell and its COM namespace. The production shell
now owns the corresponding known-folder PIDL path. It retains the existing
folder locations, the f5-15A link writer, and the NSIS fixture's original paths
and hash. Counted GUID/drive/filesystem items follow the pinned Wine 10.0
`shell32/pidl.h` and `pidl.c`; other namespaces and extended items still use
the reused parser. The native test checks Desktop and Programs, ANSI/Unicode
paths, CREATE flags, allocation ownership and malformed item rejection.

This follows Microsoft's [SHGetSpecialFolderLocation contract](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetspecialfolderlocation)
and [SHGetPathFromIDList contract](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetpathfromidlista),
and the [NSIS shell-folder context documentation](https://nsis.sourceforge.io/Reference/SetShellVarContext).
The manifest locations are unchanged; compiled NSIS shortcut creation still
requires execution evidence.

The FDI test previously invoked H1's default `fixture` mode, which disables
the installer backend. It now uses `installer-fixture` with the same copied
file/registry/system services as `installer-exec`, under the unchanged seccomp
preflight and filter. The exact extraction/corruption checks remain, with an
additional `kernel_enosys=0` assertion. The memory-file open callback also
accepts `_O_RDONLY | _O_BINARY`: pinned `cabinet/fdi.c:2475` supplies that mode,
which the callback previously rejected. This is an I/O adapter correction,
consistent with Microsoft's [FNOPEN callback](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fnopen)
and [FDICopy](https://learn.microsoft.com/en-us/windows/win32/api/fdi/nf-fdi-fdicopy),
not a change to the expected cabinet bytes. The retained pinned cabinet H1
record in `build/win32/upper/h1-load.json` identifies the eight unsupported
operations in its `msvcrt`/`cabinet` closure: `W32_FILE` (0x109) three times,
`W32_VM_QUERY` (0x6) twice, `W32_SYSTEM` (0x10a) once, and `W32_OBJECT`
(0x100) twice. All four routes already exist in the installer backend but
were disabled by the old test launch mode. This matches the lead's aggregate
count; the lead's new fixture run must confirm the per-operation results.
H1 now logs operation IDs and statuses and retains per-operation ENOSYS
totals for that run.

The INF fault's original log has no EIP or stack words. An H1 report hook now
retains EIP/ESP/EBP, volatile registers, 32 words bounded by the owned stack,
and recent kernel/registry records. Source inspection identifies a definite
invalid call in the authored stub: its first `SetupInstallFromInfSectionA`
call sets `SPINST_FILES` but supplies a NULL callback and context. The
[documented callback requirement](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfrominfsectiona)
is explicit. Wine `install.c:1091` wraps that pointer in
`QUEUE_callback_WtoA`; `queue.c:1384` sends `SPFILENOTIFY_STARTQUEUE`, and
`queue.c:286` calls the NULL original handler. This explains a NULL-call
access violation once the file queue is populated, although the lead must
still confirm the actual fault context and complete flow.

The stub now obtains the reused `SetupInitDefaultQueueCallback`,
`SetupDefaultQueueCallbackA` and `SetupTermDefaultQueueCallback` exports,
passes the real callback/context for file operations, and closes the context
after success or failure. A native UBSan check validates the actual caller
helper, including all three section failures and callback allocation failure.
No production workaround for invalid callback arguments was added, and no
file/registry/INI expectation was weakened. Only the authored `infchain.exe`
artifact hash changes; NSIS, CAB, INF and self-extractor bytes stay pinned.

A separate native UBSan test exercises
the actual `h1_services.c` endpoint with low-address native storage, the real
copied file transport, VM allocate/query/protect/free, and the INF read-only
mapping after its section and file handles close. That mapping lifetime check
passes. Neither native check is evidence of actual reused PE execution.

The mapping check matches the pinned `setupapi/parser.c:953–961` and Microsoft's
[CreateFileMapping](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga)
and [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile)
contracts. The [SetupInstallFromInfSection flags](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfrominfsectiona)
and [Wine install engine](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/setupapi/install.c)
were rechecked; their file/registry/INI expectations were preserved. Web reads
of Wine's `cabinet/fdi.c` and `advpack/install.c` failed with cache misses;
their pinned local sources were used for comparison. Current commands,
results and the required lead rerun are in `build/codex/f5-15a2-report.md`.
## Amendment 8: H1 protocol diagnostics and native conformance

The lead's `build/lead/nsis-hang-faulthandler.log` shows the Python worker
waiting for a frame header while `Child.finish()` waits for the guest. It
contains neither a request trace nor the guest's read buffer/count. That dump
alone does not identify an opcode or establish which side lost framing.

The C/Python audit checks the 16-byte `<4I` request header (magic, kind,
payload size, reply capacity), the 8-byte `<iI` reply header (status, payload
size), and the complete copied payload. Kind 1 records use FILE/PROCESS/OBJECT
72-byte headers, WAIT 56 and SYSTEM 48. FILE strings occupy fields 6/7 and
the buffer fields 8/9; PROCESS strings occupy 4/5/6 and environment 7/8;
OBJECT names occupy 11; WAIT handles/count occupy 4/5. Kind 2 retains the
existing w32srv ABI and kind 3 has an empty request body. A zero-length
payload is a valid message, distinct from EOF. Normal and Win32-error
responses preserve the entire copied record; transport errors have no body.

Native `h1_protocol_test.c` links the actual `h1_services.c` encoder, runs
on a low-address native stack with UBSan, and exchanges records with the
actual Python decoder. It covers FILE actions 1–21, OBJECT 1–13, WAIT,
PROCESS (including a native child and wait), SYSTEM 1–11, registry 1–12 and
startup. Partial transfers and EINTR are injected at the syscall adapter.
The changed protected-store checks and descriptor metadata after rename and
share-delete unlink are exercised through the C encoder. This is protocol
evidence, not NSIS/PE execution evidence. No encoder/decoder disagreement
has been reproduced by these cases. Both registry enumeration operations
return their terminal `259` inside a complete normal reply; no outer frame
or body is lost.

The actual fixture defect is the new `EnumRegKey` loops in `fixture.nsi`.
They test `IfErrors` for termination, but NSIS 2.51's
[`EW_REGENUM` implementation](https://raw.githubusercontent.com/kichik/nsis/v251/Source/exehead/exec.c)
initializes the output to empty and ignores the `RegEnumKey` return code.
In contrast, its `EnumRegValue` branch sets the error flag on enumeration end.
The official [EnumRegKey and EnumRegValue contracts](https://nsis.sourceforge.io/Docs/Chapter4.html#4.9.2.5)
document this distinction. Both fixture key-search loops now branch on empty
output as well as errors. No change is made to the API's enumeration errors
or to the H1 encoder. The stale authored fixture trees from the lead's run
contain payload, child, uninstaller and both shortcuts but no fixture Uninstall
key, consistent with the first key-search loop immediately after shortcut
creation. This is a source-backed explanation of the observed timeout, not
new PE execution evidence; the lead must rerun the flows outside the sandbox.

The service now applies one absolute 30-second receive deadline to header
and body together, records partial bytes/counts plus the last served operation
and reply, and attempts one error reply for malformed, truncated or timed-out
frames. It closes incomplete streams rather than treating late body bytes as
a new header. A complete request whose handler raises receives one error
response; the next complete frame remains aligned. `finish()` attaches the
same diagnostics to a process timeout. Reply writes also have a deadline.

These choices follow Python's official [poll documentation](https://docs.python.org/3.14/library/select.html#polling-objects)
and [socket timeout/nonblocking documentation](https://docs.python.org/3.14/library/socket.html#notes-on-socket-timeouts).
Descriptor `os.read`/`os.write` remain the transport, with readiness polling
and nonblocking descriptors; direct socket send operations are refused by this
sandbox. The absolute deadline is checked on every fragment, so partial
progress cannot extend it. Microsoft's [GetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfileinformationbyhandle)
and Python's [fstat](https://docs.python.org/3.14/library/os.html#os.fstat)
support keeping descriptor metadata separate from pathname authority checks.
The [NSIS command-line reference](https://nsis.sourceforge.io/Docs/Chapter3.html)
and [scripting reference](https://nsis.sourceforge.io/Docs/Chapter4.html)
were rechecked: `/S`, `_?=` and the installer manifests retain their contracts.

## Amendment 10: fixed loader extent

The safe-mode confirmation is shortened from 42 to 26 ASCII bytes:
`Safe mode; P probe or wait`. The host menu model uses the same text.
This recovers 16 bytes before the loader's aligned data, covering the
12-byte overflow without changing any selector name, table order, menu
action or deadline. The 24,576-byte file and its last 32-byte progress
trailer retain their fixed offsets (`docs/design/boot-memory.md`).

Research: NASM's [DB and TIMES documentation](https://www.nasm.us/doc/nasm03.html#section-3.2)
defines the emitted string bytes and expression-based padding; its
[ALIGN documentation](https://www.nasm.us/doc/nasm06.html#section-6.15.1)
explains the 16-byte alignment before `boot_info`. SeaBIOS 1.16.3's
[keyboard implementation](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c)
confirms that INT 16h AH=01 polls and AH=00 consumes input, matching the
unchanged `menu_getc` path. Decision: compact only display text, keeping
the installer/GL selectors and their dispatch and evidence names intact.
The existing assembled-loader test also checks the progress trailer bytes
at offset 24,544 and the model's CR/LF/NUL-terminated confirmation.
Exact host verification commands and results are recorded in
`build/codex/f5-15a2-report.md`, section "Amendment 10".
The 30 required gate checks and 20 menu/registration checks pass. Standalone
NASM assembly with `CIUKI_VERSION='0.9.2 pre-alpha'` emits 24,576 bytes;
`image_end` is `0x5FDC`, leaving four padding bytes before the trailer at
`0x5FE0`. No image build or guest execution is part of this evidence.
