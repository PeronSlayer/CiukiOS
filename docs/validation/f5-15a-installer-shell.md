# F5-15A installer shell: implementation decisions

Date: 2026-10-20. Host evidence only; the guest gate remains unrun.

The directive and AGENTS.md were read in full. The directive file in this
checkout has 17 lines and no appended amendment/lead-note section. The
f5-07A, f5-09/09B, f5-10C/10C2, f5-14B and f5-20B/20C reports were inspected
in the lead checkout. F5-07B restart/DDE and F5-15B original payloads are
separate worktrees and are not implemented here.

Primary references checked against the repository and implementation:

- Microsoft [Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
  and [IPersistFile](https://learn.microsoft.com/en-us/windows/win32/api/objidl/nn-objidl-ipersistfile):
  preserve target, arguments, working directory, description, show state and
  icon index. The f5-09 handoff chooses **CIUKLNK3**. It does not choose
  MS-SHLLINK; this implementation does not claim Windows binary link support.
- [SHGetSpecialFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetspecialfolderpatha):
  CSIDL lookup returns drive paths; create is explicit. Common folders alias
  the single-user folders. Startup is a folder, never an execution policy.
- [SHFileOperation](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shfileoperationa)
  and [SHFILEOPSTRUCTA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-shfileopstructa):
  double-NUL path lists and the WORD flags field. The
  [upstream shellapi.h](https://raw.githubusercontent.com/mingw-w64/mingw-w64/master/mingw-w64-headers/include/shellapi.h)
  applies pack(1) on i386: size 30, aborted at offset 18,
  separate aborted output, recursive operations, explicit unsupported undo
  and name-mapping requests rather than silently destroying undo information.
- [ShellExecuteEx](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecuteexa):
  success instance greater than 32; retain the process handle only when
  SEE_MASK_NOCLOSEPROCESS is requested. Verbs resolve through registry
  associations, with no host shell invocation. URLs are refused and recorded.
- [MoveFileEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa):
  the directive's delayed operation is represented as destination=source
  entries, NUL=source for deletion, in WININIT.INI. Preserve duplicate NUL
  entries. **Review required:** the existing contract instead specifies the
  Windows 98 error-120 behavior for this flag. The written directive takes
  precedence for this implementation; restart processing stays in f5-07B.
- [AddFontResource](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-addfontresourcea):
  installed fonts and system faces must be addressable in the Windows view.
  Stage the exact pinned system bytes at WINDOWS/FONTS with the FAT read-only
  bit; additionally protect those resolved nodes and aliases from Win32
  mutation. Installed faces remain writable. The rasteriser uses its existing
  native system store, so no second in-memory face cache is introduced.
- [SetupInstallFromInfSection](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfrominfsectiona)
  and the [NSIS fixed-media DirectSetup example](https://nsis.sourceforge.io/DirectX_Installation_for_Fixed_Media):
  INF and DirectSetup must be measured as flows; successful DLL binding or one
  success-return function is insufficient evidence.

The owner's local Windows 98 Resource Kit book was inspected at
`build/rskt/` in the lead checkout: `wrkc05_8.htm` (Troubleshooting Setup,
shell-folder registry troubleshooting) and `wrkappd_1.htm`/`wrkappd_3.htm`
(INF shell-folder registry entries and Start Menu link reconciliation).
This is local licensed research material; none is copied into this worktree.
Modern Microsoft API pages supply ABI documentation, not evidence that NT
restart mechanisms describe Windows 98.

The Resource Kit's `wrkc31_10.htm`, "Error When Removing a Program with
Add/Remove Programs", explicitly identifies
HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall. Its discussion
of stale entries agrees with the f5-09 page reading installer-written data;
there is no CiukiOS-generated Uninstall record.

Additional comparisons: Microsoft's
[VerInstallFile](https://learn.microsoft.com/en-us/windows/win32/api/winver/nf-winver-verinstallfilea)
defines the flags, source/destination filename fields, version comparison and
temporary-file lifecycle. The delivered implementation's protected-builtin
copy policy is required by CiukiOS; its compressed/temp-file mismatch paths
are still incomplete and must not be presented as full API conformance.
[Dirids](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-dirids),
[DestinationDirs](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-destinationdirs-section),
[SourceDisksFiles](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-sourcedisksfiles-section)
and [AddReg](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-addreg-directive)
were checked for the authored INF source. LDID 11 is the personality's SYSTEM
directory, not an adoption of NT's SYSTEM32. The reused Wine INF path still
requires actual runtime tests; an authored INF alone is not that evidence.

`ciukishell.dll` is an authored support module. The loader forwards an explicit
list of platform-owned shell32 named exports to it, using its existing module
dependency/lifetime machinery. Other shell32 exports remain supplied by the
reused Wine build when qualified and staged. The original Wine PE bytes,
build hashes, and qualification flags are not changed. The support DLL is
also usable with the present census placeholder. The lead must review this
integration seam; it is not a qualification of Wine's other shell services.
The private `__ciuki_reused_` lookup prefix permits the support class factory
to delegate non-ShellLink classes to Wine's original factory without recursion.

All desktop PE launches now require endpoint evidence, including after a
supervised server restart. The shortcut worker uses a read-only registry INFO
round trip with a five-second bounded wait for an absent service; the v3
fixture defers spawn to its tick after an OPEN/CLOSE round trip, matching
the existing dialogs fixture. No new Win32 process is launched to establish
readiness. Host registry and lifecycle tests check success/refusal paths.
