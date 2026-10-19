# F5-16 compatibility and diagnostic boundary

Research, 2026-10-19, before implementation:

- Microsoft [GetVersion](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getversion)
  and [GetVersionEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getversionexa):
  version shims change reported identity, including the platform and build.
  [VerifyVersionInfo](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-verifyversioninfoa)
  and [VerSetConditionMask](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-versetconditionmask)
  require hierarchical major/minor/service-pack comparison and ERROR_OLD_WIN_VERSION.
- Microsoft [PE/COFF](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  match the original file, not relocated pages; timestamp and image size are
  independent fields. A SHA-256 mismatch must not fall back within that entry.
- Microsoft [DLL search](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order):
  preserve loaded-module identity; native preference only affects named non-core
  DLLs, before the builtin search, and never substitutes ntdll/kernel32/user32/gdi32.
- Microsoft [HeapAlloc](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapalloc)
  and [GlobalMemoryStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-globalmemorystatusex):
  padding does not change requested HeapSize, zeroing covers new bytes, reported
  availability never exceeds reported totals. No physical memory limit changes.
- Intel [SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  (IN/OUT, x87 control word) and [8254 timers](https://edc.intel.com/content/www/pl/pl/design/ipla/software-development-platforms/servers/platforms/intel-pentium-silver-and-intel-celeron-processors-datasheet-volume-1-of-2/005/8254-timers/):
  emulate scalar byte I/O only, with a virtual channel-0 latch; reject other
  ports/instruction forms without advancing EIP. Never touch real devices.
- Microsoft [SEH](https://learn.microsoft.com/en-us/windows/win32/debug/structured-exception-handling),
  [system errors](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-),
  [HRESULTs](https://learn.microsoft.com/en-us/windows/win32/seccrypto/common-hresult-values)
  and [CreateService](https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-createservicea):
  unhandled faults retain their original status; BOOL/handle stubs fail with 120,
  HRESULT stubs return E_NOTIMPL, MMRESULT 8, LSTATUS 120. Driver service creation
  fails with ERROR_ACCESS_DENIED (5).
- Pinned [Wine 10.0 kernel32 export specification](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/kernel32/kernel32.spec)
  and the corresponding checked-in DLL specs provide i386 argument widths and
  calling conventions. They do not provide return types. Explicit CiukiOS
  failure metadata is kept separately in `scripts/win32/stub_signatures.py`.
  Microsoft [OpenFile](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-openfile)
  and [RealizePalette](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-realizepalette)
  require -1 failures; zero can be successful. [DdeInitializeA](https://learn.microsoft.com/en-us/windows/win32/api/ddeml/nf-ddeml-ddeinitializea)
  uses zero for success; its refusal returns DMLERR_SYS_ERROR, 0x400f, from
  Microsoft's [DDEML header](https://raw.githubusercontent.com/microsoft/win32metadata/main/generation/WinSDK/RecompiledIdlHeaders/um/ddeml.h).
  [GetDeviceCaps](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-getdevicecaps),
  [CharNextA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charnexta),
  [CharLowerA](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-charlowera)
  and [ImmAssociateContext](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immassociatecontext)
  expose data or prior state without an unambiguous failure value; their
  remaining stubs stay nonreturning. [SelectObject](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-selectobject)
  has object-dependent failure values, so its generic stub also stays fatal.
  [Microsoft multimedia functions](https://learn.microsoft.com/en-us/windows/win32/multimedia/multimedia-functions)
  and [RegOpenKeyEx](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regopenkeyexa)
  distinguish MMRESULT and LSTATUS errors from BOOL and HRESULT.
- Windows 98 Resource Kit, chapter 27, `wrkc27_2.htm`, “Using Dr. Watson”
  (owner's extracted book, main checkout `build/rskt/`), and chapter 17,
  `wrkc17_17.htm`: the illegal-operation wording, Details, module/address and
  automatic numbered fault logs. CiukiOS deliberately records only registers
  and eight safely read stack words, never Dr. Watson's broader snapshot.

Repository comparison: f5-03 already owns exception delivery; f5-05B keeps
immutable EXE snapshots and loader failure reasons; f5-06 owns process names
and version/memory facades; f5-09 owns the desktop dialog. Generated stubs
previously raised STATUS_NOT_IMPLEMENTED and the kernel shared a 64-record
stub budget. F5-16 replaces known signatures with typed failures and separates
once/export stubs from the 64-warning budget. Unknown signatures remain
nonreturning, fail-closed refusals and must be listed in the audit.

## Database and override schema

ASCII text, at most 64 KiB, 1024-byte lines, at most 256 entries. CRLF and LF
are accepted. Blank lines and whole-line `;` or `#` comments are ignored.
Duplicate executable sections are permitted (different fingerprints).
Keys are case-sensitive; executable names compare without case and contain no
directory. Each section has `shims=` and at least one match key:
`TimeDateStamp`, `SizeOfImage`, `file-size` (decimal or `0x` hexadecimal),
or `sha256` (64 hexadecimal digits). Duplicate/unknown keys, bad numbers,
unknown names/versions and invalid parameters reject the entire input layer.
All specified fingerprints must match. Specificity is SHA-256, then both PE
fields, then one PE field, then file size. On equal specificity the first
entry wins. Multiple matching entries with different settings produce a
conflict warning; the most specific wins. An override match wins over the
file regardless of specificity; `shims=` may be empty to disable file shims.

The Control Panel stores one REG_BINARY value `Database` below
`HKLM\Software\CiukiOS\Win32\AppCompat`, containing the same bounded ASCII
grammar (without a NUL). The reader takes an immutable startup copy. This is
a private schema, not Windows AppCompat registry emulation. Only the desktop
owner may mutate this subtree through authenticated w32srv RPC.

Shim versions start at 1; behavior changes require a version bump and title
requalification, as required by the personality contract. Tests pin the table.
Parameters: `version@1(95|me|nt4|2000|xp)`, `heap-pad@1(bytes)` (0..65536),
`memstatus-cap@1(MiB)` (1..4096), `fpu-cw@1(word)` (valid x87 bits/precision),
`ddraw-scale@1(1..8|mode:WIDTHxHEIGHT)`, `dll-native@1(a.dll|b.dll)`,
`w-stubs@1(ExportW|OtherW)`. The other shims take no parameters. Commas
separate shims. Lists are bounded and duplicate shims are rejected.
DirectDraw entries remain declared with an explicit unhooked warning when
f5-11's implementation is absent.
