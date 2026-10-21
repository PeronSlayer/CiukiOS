# F5-15A2 installer qualification

Research date: 2026-10-21. Host evidence only. Exact commands, outputs and
remaining execution limits are in `build/codex/f5-15a2-report.md`.

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

The baseline H1 adapter additionally lacks general file, registry and child
process transports (its intended original scope is DLL attach/pure entry
tests). A complete four-flow host NSIS driver and these transports remain
necessary; the authored guest driver is not a replacement for that evidence.
The four self-deleting patterns are not fully qualified. This delivery must
not be treated as closing all f5-15A2 acceptance criteria.
