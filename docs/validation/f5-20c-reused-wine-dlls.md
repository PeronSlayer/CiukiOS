# f5-20C: reused Wine 10.0 upper DLLs — patches, rebuild, load, DllMain, conformance

Directive `docs/directives/f5-20-core-exports-for-reused-dlls.md` part C, with
amendments 1–2 and the f5-14B amendment 2 (icon trim). Machine record:
`config/wine-upper.json` (schema 2); generated tables:
`docs/design/win32-wine-dependency-audit.md` (block "GENERATED WINE UPPER").
Host evidence only: no QEMU, no image build (`w32-upper-dlls-qemu-t23` stays
unrun).

## Sources

- Wine 10.0 as built (archive SHA-256 `c5e0b3f5…3601`, commit
  `b073859675060c9211fcbccfd90e4e87520dc2c2`): `dlls/{shlwapi,comctl32,ole32,
  setupapi}/Makefile.in` and `.spec` (`-import` entries), `dlls/kernelbase/{path,
  string,main,registry,locale}.c`, `dlls/shell32/{shfldr_fs,recyclebin}.c`,
  `dlls/msvcrt/{main,locale,mbcs,lock}.c`, `dlls/riched20/richedit.c`,
  `tools/makedep.c` (`get_default_imports`: KERNEL32 and NTDLL are default
  imports), `include/wine/test.h` (conformance framework, output through
  `vprintf`, exit code = failures).
- `_getmbcp`, "A return value of 0 indicates that a single byte code page is
  in use": https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/getmbcp
- `InitializeCriticalSectionEx` (Vista+; documented flag
  `CRITICAL_SECTION_NO_DEBUG_INFO`; FALSE and GetLastError on failure):
  https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-initializecriticalsectionex;
  `RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO` 0x10000000 and the
  `RTL_CRITICAL_SECTION_DEBUG` layout: Wine 10.0 `include/winnt.h`.
- DllMain (FALSE on attach makes LoadLibrary fail; the system then calls
  DLL_PROCESS_DETACH): https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain
- Failure values of the W21 stub DLLs: OpenThemeData (NULL)
  https://learn.microsoft.com/en-us/windows/win32/api/uxtheme/nf-uxtheme-openthemedata;
  AcquireCredentialsHandle (SEC_E_SECPKG_NOT_FOUND)
  https://learn.microsoft.com/en-us/windows/win32/api/sspi/nf-sspi-acquirecredentialshandlea;
  GetAdaptersInfo (ERROR_NOT_SUPPORTED)
  https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getadaptersinfo;
  GDI+ `Status` (NotImplemented = 6)
  https://learn.microsoft.com/en-us/windows/win32/api/gdiplustypes/ne-gdiplustypes-status;
  CryptCATAdminAcquireContext (FALSE)
  https://learn.microsoft.com/en-us/windows/win32/api/mscat/nf-mscat-cryptcatadminacquirecontext.
- Census W21 stubs added for the reused DLLs: DeviceCapabilities (-1)
  https://learn.microsoft.com/en-us/windows/win32/printdocs/devicecapabilities;
  DocumentProperties (< 0)
  https://learn.microsoft.com/en-us/windows/win32/printdocs/documentproperties;
  EnumPrinters (FALSE) https://learn.microsoft.com/en-us/windows/win32/printdocs/enumprinters;
  ImmGetCompositionString (negative IMM_ERROR_*)
  https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetcompositionstringw.
- seccomp filters see the instruction pointer (`struct seccomp_data`):
  https://man7.org/linux/man-pages/man2/seccomp.2.html

## Decisions taken in part C

1. Patches 0006–0010 as listed in `third_party/wine/PATCHES.md`. 0006 is a
   recorded resource filter (binary inputs); it brings SHELL32 under the
   loader's 4,096-entry resource walk, so the loader bound stays 4,096.
2. The compiled-in kernelbase units must not add NT-only imports to SHLWAPI:
   RtlDowncaseUnicodeChar (UrlCombineW) goes through CharLowerW and
   IsDBCSLeadByteEx (a private CharNextExA copy) through GetCPInfo, in the
   wrapper units of patch 0008.
3. The 57 delay-loaded entries of absent DLLs (uxtheme, secur32, gdiplus,
   urlmon, wintrust, iphlpapi, mlang, shdocvw, windowscodecs) are nine
   generated CiukiOS W21 stub DLLs in the builtin store
   (`scripts/win32/upper_dlls.py ABSENT_DELAYED`, inventory
   `absent-dll-stubs-inventory.json`), each export logging one W21 diagnostic
   and returning the documented failure; the three lazy forwarders of SHLWAPI
   (userenv ×2, wininet) stay decided `w21-unresolved-forwarder`.
4. The 17 `ciukios-stub-dll` decisions (winspool.drv 12, imm32 3, mpr 2),
   which neither part A nor part B owned, are typed W21 stubs of the existing
   census DLLs (`stubs.generate(extra=...)`); without them COMCTL32, COMDLG32
   and RICHED20 did not bind.
5. The record keeps the f5-14B table (audit `f5-14b`, each row with its
   closure in this build), adds rows for the f5-20B exports outside it (audit
   `f5-20b`: user32 87, gdi32 4, derived from the pre-f5-20B compiled exports)
   and one row per entry of the re-audit (`f5-20c`: imports still missing,
   and imports bound to core W21 stubs without an f5-14B decision). The A/B
   generators and their tests read only the f5-14B table.
6. The per-application `w-stubs` shim gates every exported stdcall Unicode
   function of USER32 and GDI32 (amendment 2 item 7), with the KERNEL32
   mechanism; the implementations are renamed in every object, so calls inside
   the DLL stay ungated.
7. Core fixes found by the H1 DllMain run: `InitializeCriticalSectionEx`
   honours `RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO` (allocates an owned
   `RTL_CRITICAL_SECTION_DEBUG`; Wine's MSVCRT, COMBASE, RPCRT4, COMCTL32 and
   OLE32 name sections through `DebugInfo->Spare[0]`), and
   `RtlDeleteCriticalSection` frees exactly the owned blocks. MSVCRT's
   C-locale code page is patch 0010.
8. Lead decision: no reused PE is staged yet. MSVCRT has the H0 binding and
   H1 DllMain evidence, and `scripts/build_image.py` stages any PE listed in
   `wine_upper.QUALIFIED` (fail-closed on a missing or changed PE), but the
   list stays empty until a cached upper build is part of `make build-full`.
9. Lead decision: KERNEL32 IsBadReadPtr/IsBadWritePtr/IsBadCodePtr (range
   probed through VirtualQuery, TRUE on any inaccessible page, never faulting)
   and DebugBreak (int 3, delivered as EXCEPTION_BREAKPOINT by the kernel's
   vector-3 path) replace the nonreturning census stubs that comctl32, ole32,
   msvcrt and oleaut32 bound (decision rows `core-api-f5-20c`).

## H1 adapter

The i386 Linux adapter runs the production loader, the production core PEs
and the reused PEs; every DllMain executes. The PE ntdll is the adapter build
with `tests/host/win32/wine/h1_transport.c` instead of the CiukiOS transport:
the host answers the kernel's W32_IDENTITY record (Windows 98 4.10.2222, as
`src/kernel/proc/win32/records.c`), the w32srv atom directory
(ATOM_ADD/FIND/DELETE/NAME with the `apps/w32srv/directory.c` rules) and, for
conformance programs, a console on the standard handles; every other kernel
record fails with ENOSYS and every other w32srv operation with 120, counted
per run. A seccomp filter allows system calls only from the adapter ELF's
text, so PE code (CiukiOS `int 0x80` gates) never reaches Linux. Builtin
views are not unmapped by the loader, as in the production services.

Results: `config/wine-upper.json` (`load`, `conformance`) and the generated
tables.
