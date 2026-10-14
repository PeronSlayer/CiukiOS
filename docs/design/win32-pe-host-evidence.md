# f5-02A PE tools and host evidence (2026-10-14)

This is the host preparation milestone of the reviewed Win32 contract. It
introduces no guest loader, runtime API, architecture decision or scope change.

Research consulted before implementation:

- [Microsoft PE/COFF format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  PE32 directory counts are bounded by both optional-header size and declared
  count; RVAs differ from file offsets; the certificate directory uses a file
  offset. Imports, ordinal flags, bound imports, delay descriptors, exports,
  TLS and the three-level resource tree are decoded separately.
- [Microsoft i386 calling conventions](https://learn.microsoft.com/en-us/cpp/cpp/argument-passing-and-naming-conventions?view=msvc-170):
  use actual i386 COFF builds for stack cleanup, ECX this pointers, EDX:EAX
  returns and callee-preserved EBX/ESI/EDI/EBP. Host compilation/disassembly
  supplies ABI evidence; it does not execute Windows exception dispatch.
- [LLVM Windows linker support](https://lld.llvm.org/windows_support.html) and
  [LLVM deterministic builds](https://blog.llvm.org/2019/11/deterministic-builds-with-clang-and-lld.html):
  freestanding clang + lld-link supports DEF import libraries, DLLs and RES
  resources without a CRT. Use a fixed timestamp, no debug paths, no incremental
  linking and two separate output directories to test reproducibility.
- [MinGW-w64 upstream changelog](https://www.mingw-w64.org/changelog/):
  version 12 defaults to UCRT. Future CRT builds must explicitly select MSVCRT;
  the initial corpus has no CRT imports and requires no downloaded headers.
- [NSIS upstream history](https://nsis.sourceforge.io/Docs/AppendixF.html):
  NSIS 2.0 was released 2004-02-07. A current NSIS 3 fixture would not qualify
  the requested period installer; NSIS 2 remains optional preparation.
- [pefile upstream parser](https://raw.githubusercontent.com/erocarrera/pefile/master/pefile.py),
  `parse_delay_import_directory` and `get_import_table`: legacy attributes-zero
  i386 delay descriptors and their nonordinal thunks can contain VAs, not RVAs.
  Normalize both; preserve ordinal flags. Fixtures cover both descriptor forms.
  An empty original-first-thunk falls back to the IAT as the contract requires;
  a delay IAT with no name table remains unresolved rather than interpreting
  helper code as API strings.
- [Original Quake II renderer loader](https://github.com/id-Software/Quake-2/blob/master/win32/vid_dll.c):
  static imports cannot enumerate every dynamically selected renderer. Quake II
  remains a proposed additional workload, not a replacement approved gate.
- [Microsoft LoadLibraryExA](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexa)
  and [DLL entry functions](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-entry-point-function):
  the builtin-import probe uses ordinary explicit loads/reference counts and
  DATAFILE flag 2 at its EXE entry, not recursive LoadLibrary inside DllMain.
  Its minimal kernel32 DEF is a fixture import surface, not a frozen API census.

Decisions for this implementation: preserve native toolchain keys, add a
separate Win32 pin block, use a dependency-free bounded PE32 parser, keep the
fixture expected outcomes in source JSON independently of the parser, and
publish names/metadata only. Exact parser refusal reasons are T0 diagnostics
with intended future ERROR_BAD_EXE_FORMAT (193) mapping; they are not a claim
that CiukiOS already rejects a PE at runtime. Runtime expectations in fixture
manifests remain pending T3 evidence. No Windows/Wine reference runs occurred.

The existing contract has an unresolved contradiction: it forbids virtual
section overlap and later asks for Windows-compatible overlapping layouts.
The corpus distinguishes non-overlapping low/page-shared alignment from an
explicit overlap negative. Lead disposition and original Windows reference
measurements are required before treating the latter as a frozen guest policy.
Period SDK/DDK archives were not available or consulted; current Microsoft
format/ABI documents are structural sources, not substitutes for the period
DirectX 7.0/8.1 topics requested by the contract. That audit remains pending.

MinGW-w64 **10.0.0** is pinned for headers/CRT preparation (the directive does
not mandate a release number). Its source archive's exact SHA-256 is published
on the [upstream download page](https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/mingw-w64-v10.0.0.tar.bz2/download).
The exact size, 9,620,291 bytes, is recorded by the
[archive mirror index](https://ftp.lysator.liu.se/pub/void-ppc-sources/cross-x86_64-w64-mingw32-10.0.0/).
The pinned upstream URL could not be downloaded because sandbox DNS failed.
The verifier checks both size and hash when the lead supplies the archive;
it does not treat a pending archive as installed headers or a working CRT.
Select MSVCRT explicitly for subsequent CRT qualification. NSIS 2.0 remains an
optional requested sample, with no invented archive hash or installer evidence.
