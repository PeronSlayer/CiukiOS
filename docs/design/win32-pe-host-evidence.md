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

## 2026-10-14 — f5-05A loader and ntdll service boundary

Research consulted for the implementation:

- [Microsoft PE/COFF](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  use independent bounded walkers for RVA tables, file-offset certificates,
  relocation pairs, delay descriptors and TLS VAs. Match f5-02 diagnostics;
  map zero-filled tails and union permissions on shared pages. Bound imports
  are descriptive and never trusted as executable pointers. Preserve delay
  IATs for the application's helper.
- [Wine 11.18 x86 dispatcher](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.18/dlls/ntdll/signal_i386.c)
  and [Microsoft RtlUnwind](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlunwind):
  validate the FS registration chain against stack limits, distinguish search
  and unwind dispositions, protect handler calls with nested registration
  records, reject noncontinuable continuation, and leave the target frame
  installed. CONTEXT uses the real i386 layout. Linux signals supply host
  fault contexts; the kernel remains responsible for safe context restoration.
- [Wine public layouts](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.18/include/winternl.h):
  pin the era-compatible TIB/TEB/PEB prefixes, process parameters and loader
  lists with i386 static assertions. Internal extension fields are not a
  frozen CiukiOS syscall ABI.
- [Microsoft heap allocation](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapalloc):
  preserve eight-byte alignment, zero-size allocations, optional zero fill,
  serialization and allocation failure. Debug tail/free checking is opt-in;
  ordinary free preserves payload bytes. External heap regions and tuning
  parameters require section ownership and are explicitly refused for now.
- [Wine loader](https://raw.githubusercontent.com/wine-mirror/wine/wine-11.18/dlls/ntdll/loader.c):
  resolve imports before initialization; track dependencies and initialization
  order separately from load order, and roll back a failed load transaction.
  The implementation below is original code, not copied Wine code.
- [Wine wsock32](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/wsock32/wsock32.spec),
  [comctl32](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/comctl32/comctl32.spec)
  and [dplayx](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/dplayx/dplayx.spec):
  retain explicit census ordinal assignments. [Wine winmm](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/winmm/winmm.spec)
  has no ordinal 2 (research section 3); its Windows identity and calling convention remain
unqualified: reserve an opaque NONAME stub at 2, never alias it to PlaySoundA.

Implementation decisions within the directive: use private `w32_services`
callbacks until the missing f5-03a record header is frozen; build the same core
for Linux i386 and both configured PE targets, without libc or SSE dependencies.
The CiukiOS service table is intentionally unbound and cannot claim guest
acceptance. Unknown-ABI generated stubs diagnose once and raise a noncontinuable
STATUS_NOT_IMPLEMENTED, so missing implementations load without returning with
guessed stack cleanup. They do not establish that a title reaches WinMain.

The existing virtual-overlap negative remains a refusal pending lead disposition.
A bound IAT with missing original symbols fails with `bound-iat-without-names`
and ERROR_BAD_EXE_FORMAT rather than using stale addresses. This is a proposed
fail-closed disposition of an explicitly unresolved fixture, not a new owner
compatibility decision. Synthetic cyclic/forwarder scenarios all reuse base
0x400000 without relocations; tests must distinguish their real collision from
derived distinct-base graph fixtures. Synthetic entry points are structural
probes and use an explicit observer; compiler fixtures execute real i386 code.

## 2026-10-15 — f5-05A completion on the Linux i386 adapter

Further sources consulted:

- [Microsoft DllMain](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain):
  `lpvReserved` is non-NULL for static loads and for the detach at process
  termination, NULL for `LoadLibrary`, `FreeLibrary` and failed loads; a
  `FALSE` attach during `LoadLibrary` is followed by `DLL_PROCESS_DETACH` and
  unload; existing threads get no `DLL_THREAD_ATTACH` for a later load.
  The loader now passes `lpvReserved` accordingly.
- [Microsoft EXCEPTION_RECORD](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record)
  and [RtlUnwind](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlunwind):
  a handler that continues a noncontinuable exception, or returns an invalid
  disposition, receives a new noncontinuable `STATUS_NONCONTINUABLE_EXCEPTION`
  / `STATUS_INVALID_DISPOSITION` whose `ExceptionRecord` links the original.
- [Microsoft FindResourceEx](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-findresourceexa)
  and [HeapReAlloc](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc):
  resource language fallback and `HEAP_REALLOC_IN_PLACE_ONLY`/`HEAP_ZERO_MEMORY`.

Decisions taken inside the directive (original code, no Wine source copied):

- Heap: segregated free lists (64 exact 8-byte bins, power-of-two bins above),
  header-validated block lookup (magic, owning segment, neighbour links)
  instead of a whole-heap walk, requests of 0x7F000 bytes or more in their own
  segment, wholly free extra segments returned to the system. Freed payload is
  still preserved unless free checking is enabled. `RtlWalkHeap` reports a
  segment entry, then busy (`1`) and free (`0`) blocks.
- Exceptions: the adapter delivers host faults as the contract's W7 does
  (record and context on the user stack, EIP at `KiUserExceptionDispatcher`
  of the core in use, PE `ntdll.dll` included). `int3` reports the int3 byte
  both in `ExceptionAddress` and `CONTEXT.Eip`; privileged instructions give
  `0xC0000096`; `#MF` codes come from the x87 status word.
- Ldr/Rtl surface: `NtCurrentTeb`, `LdrFindResource_U`,
  `LdrFindResourceDirectory_U`, `LdrAccessResource`, `RtlImageNtHeader`,
  `RtlImageDirectoryEntryToData` exported. Resource language order: requested,
  primary-language neutral, neutral, 0x409 (until f5-06 supplies the user
  locale), English, any. Rtl string buffers come from the process heap.
- Process start: `w32_ldr_start_process` maps the EXE first, creates the
  process heap and returns the exit status (0xC0000135, 0xC0000139,
  0xC0000142, 0xC000007B); `w32_ldr_start_message` produces the contract's
  missing-DLL / missing-export texts, also recorded as a `dialog` diagnostic.
- The CiukiOS service backend is fail-closed (every resource request fails
  with a named refusal) until the f5-03a record header is frozen.

Wine 11.18 oracle results on self-authored probes (`oracle.c`, `seh_probe.c`):
every heap, string, path, SEH dispatch, unwind, nested-call and fault check
agrees, with one recorded difference: for a handler that continues a
noncontinuable exception, Wine passes `ExceptionRecord == NULL` in the
`STATUS_NONCONTINUABLE_EXCEPTION` record, Windows (and CiukiOS) link the
original record. The probe accepts NULL only when it detects Wine.

