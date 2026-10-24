# F5-14A — Wine 10.0 dependency and PE build audit

Date: 2026-10-14. Host-only evidence for the lead before the core ABI freeze.
The machine-readable census is `config/wine-audit.json`; the release and
tool/source pins are in `config/wine-pin.json`.

## Milestone B research and delivery boundary (2026-10-19)

Research precedes implementation. The pinned Wine 10.0 Makefiles and export
specifications for all fifteen upper candidates were re-read through the GitHub
connector at commit `b073859675060c9211fcbccfd90e4e87520dc2c2`.
Examples: [COMCTL32](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/comctl32/Makefile.in),
[COMBASE](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/combase/Makefile.in),
[RPCRT4](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/rpcrt4/Makefile.in),
[MSVCRT](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/msvcrt/Makefile.in).
The checked-in, hash-pinned snapshots remain the offline authority.

The actual [RPC transport implementation](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/rpcrt4/rpc_transport.c)
contains named-pipe, local RPC, TCP and HTTP paths; removing just WS2_32 from
IMPORTS would not disable those transports. The
[COMBASE RPCSS implementation](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/combase/rpc.c)
starts a service and contacts RPCSS. Both require real source patches and
behavioral tests before qualification. No fabricated patch or successful COM
load is recorded while the complete source tree is unavailable.

Decision: audit actual PE names and ordinals, eager and delayed imports, and
all exported forwarders recursively against actual provider PEs. Keep linked
evidence separate from the existing source-reference candidates. Missing
core exports require an explicit core amendment or an implemented Wine-side
patch; no generic zero-return substitute is an implementation decision.
The production section store eagerly seeds header/raw pages, but currently
records writable COW promises rather than allocating all private pages at
load. Report seed residency and COW commitment separately; neither includes
DllMain heaps, page tables, metadata, or whole-system 128 MiB qualification.

For DirectSetup, [Microsoft's installation documentation](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/directx-setup-for-game-developers)
distinguishes core runtime identity from optional components. The original
CiukiOS builtins implement the contract's 7.0a no-copy identity, with success
0 (no reboot); version query success is nonzero. ABI/constant cross-checks:
[upstream DirectSetup declarations](https://github.com/reactos/reactos/blob/master/sdk/include/dxsdk/dsetup.h),
[upstream compatibility shim](https://github.com/reactos/reactos/blob/master/dll/appcompat/shims/layer/forcedxsetupsuccess.c).
No implementation is copied. Version is `0x00040007`, revision 716
([Microsoft's period clarification](https://groups.google.com/g/microsoft.public.directx/c/08tNGC7hF9A)).
DirectXSetup has no requested-version parameter: recognizing later packages
requires installer preflight, and remains an explicit unqualified gate.
Unknown component flags (including Managed DirectX) return
`DSETUPERR_BADWINDOWSVERSION` (-1); the period component/UI/test flag set
returns the approved already-installed success without copying anything.

IMM32 retains the existing census-sized W21 placeholder rather than pretending
to supply an IME. Its full Wine candidate also imports WIN32U. The allowed stub
choice is not a measured size comparison against an unbuilt Wine DLL.
[ImmGetContext](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetcontext)
and [ImmGetCompositionString](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immgetcompositionstringa)
remain distinct APIs with different failure conventions.

Wine's [LICENSE](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/LICENSE),
[AUTHORS](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/AUTHORS)
and [LGPL 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html) were checked.
Distributing reused PEs requires complete corresponding source, modifications
and build instructions; upstream URLs alone do not supply that source.

The release-archive attempt on this worktree failed with curl exit 6 (DNS).
There is no full Wine source cache or original ten-title import census here.
Consequently Wine upper PE builds, patched transports/RPCSS, Wine conformance,
the MSVCRT comparison and the 128 MiB gate remain incomplete. The B tools and
report must preserve these facts and refuse unqualified reused inventory.

B host evidence: both authored DirectSetup DLLs are 2,560 file bytes and
16,384 SizeOfImage bytes, with four cold seed pages and zero writable COW
promises each. Fresh production kernel-store H0 runs peak at 77,824 bytes
versus a 45,056-byte store-only baseline (32,768-byte increment per DLL),
and return model resources to zero. PE callbacks are observed; actual i386
execution is blocked by seccomp. Two consecutive standalone build script
runs have identical hashes. These are not Wine DLL or hardware measurements.

API reference generation, apps and the unchanged kernel builder pass. The
focused upper/store/metadata checks pass (11 tests, one explicit seccomp skip).
Full Win32 discovery runs 137 tests: 124 pass, 12 skip and the unchanged
USER32 production test fails with an ASan call through address zero; that
failure reproduces alone. The source state has no staged MSVCRT PE, so its
manifest policy now says so explicitly rather than implying a placeholder
exists. Exact artifacts, missing candidate names and remaining gates are
recorded in `build/codex/f5-14b-report.md`.

## Milestone B continuation: verified archive and PE recipe (2026-10-19)

The statements above describe the first delivery. Amendment 1 staged the full
release archive; its SHA-256 is now verified locally against the unchanged
release pin. The census is regenerated from that full tree, preserving the A
version experiment and installed-Wine proxy as historical, separate evidence.
The graph has 662 nodes. Its additional Makefile/spec snapshots are pinned
from the verified archive, without replacing any previous source-file hashes.
`config/wine-audit.json` remains the canonical machine-readable audit.

Primary sources rechecked before continuation implementation:

- The fifteen pinned module Makefiles (COMCTL32, COMDLG32, SHELL32, SHLWAPI,
  OLE32, COMBASE, OLEAUT32, RPCRT4, RICHED20/32, CABINET, ADVPACK, SETUPAPI,
  IMM32, MSVCRT), their local `.spec` files and Unix usage. The
  [pinned configure](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/configure.ac#L774)
  supports `--enable-archs=i386` with clang targeting i686-windows and lld.
  Decision: use upstream dependency generation, WIDL, WRC, winebuild and
  winegcc, rather than manually guessing generated headers or IDL sources.
  Only explicit DLL targets are built, sequentially; no Wine runtime/server.
- [RPC transport source](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/rpcrt4/rpc_transport.c#L3220):
  protocol registration, tower parsing and enumeration all consult transport
  tables. Decision: remove the concrete pipe/LRPC/TCP/HTTP implementations,
  reject protocol lookup and tower parsing, and return no protocols. Preserve
  NDR, UUID, memory and in-process interface code in other translation units.
  Removing only WS2_32 from IMPORTS would leave transport paths enabled.
- [COMBASE activation](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/combase/rpc.c#L185)
  starts RPCSS and retries local-server activation for thirty seconds.
  Decision: fail RPCSS/ROT and local-server entry points immediately with
  RPC_S_SERVER_UNAVAILABLE mapped to HRESULT; keep the in-process factory,
  allocator and marshaling implementations. Cross-apartment remoting is also
  unavailable in this subset. This needs behavioral qualification, not just
  successful compilation.
- SETUPAPI's pinned devinst.c supplies hardware discovery/installation.
  Decision: the 90 implemented SetupDi entry points fail with
  ERROR_CALL_NOT_IMPLEMENTED and type-appropriate failure returns, leaving
  the INF/cabinet/file/registry/INI translation units intact.
- Wine's pinned LICENSE, AUTHORS and
  [COPYING.LIB sections 4–6](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/COPYING.LIB#L231)
  were read online and compared locally. Decision: keep these experimental
  PEs outside the builtin store until qualified. A future distribution must
  supply the verified archive, applied patches and recipe together, beside
  the binaries. URLs alone remain insufficient.

### Continuation results: builds, CRT, bases, decisions (2026-10-19)

Additional primary sources read before the second recipe revision, and the
resulting decisions:

- [makedep `get_default_crt`/`get_crt_define`](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/makedep.c#L2229):
  modules without an explicit CRT import link `ucrtbase`, and a CRT module in
  `IMPORTS` selects that CRT and `-D_MSVCR_VER`. Windows 98 has no UCRT.
  Decision: patch `0004-single-msvcrt-crt` adds `msvcrt` to the `IMPORTS` of
  every reused module, so they share the one system `msvcrt.dll`. The three
  UCRT-only calls are kept locally with their documented semantics:
  `_recalloc` ([Wine keeps it MSVCR80+](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/msvcrt/heap.c#L477)),
  `round()` (exact half-away-from-zero) and `_swprintf_l`, mapped to the
  `_snwprintf_l` export with the same arguments.
- [winegcc link arguments](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/winegcc/winegcc.c#L470): outside macOS,
  `-s` is not honoured and `-Wl,-debug:dwarf` is always appended, which keeps
  a COFF symbol table past SizeOfImage. Decision: `llvm-objcopy --strip-all`
  after linking (pinned in `config/wine-pin.json`); no mapped byte changes.
- The production loader maps a builtin only at its manifest base
  (`sdk/win32/ldr/loader.c`, `builtin-image-manifest`). Decision: each reused
  DLL is linked at a fixed base packed from `0x74000000` in recipe order with
  64 KiB granularity (the authored builtins end below `0x70550000`); patch
  `0005-builtin-region-base` removes RICHED20's upstream fixed base.
- [COML2](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/coml2/Makefile.in) and [SHCORE](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/shcore/Makefile.in)
  are eager imports of OLE32 (structured storage) and SHLWAPI (streams,
  registry, thread helpers) in Wine 10.0; Windows 98 had these inside OLE32
  and SHLWAPI. Decision: both are built as in-process pieces of the reused set.
- [Delay-load failure handling](https://learn.microsoft.com/en-us/cpp/build/reference/error-handling-and-notification):
  an unhandled delay-load failure raises `VcppException(ERROR_SEVERITY_ERROR,
  ERROR_MOD_NOT_FOUND/ERROR_PROC_NOT_FOUND)`, and Wine's
  [`DelayLoadFailureHook`](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/kernelbase/loader.c#L202) raises a
  non-continuable exception. Decision: a delay-imported DLL that CiukiOS lacks
  (UXTHEME, GDIPLUS, SECUR32, URLMON, ...) becomes a generated CiukiOS W21 stub
  DLL returning the documented failure, so a call fails instead of faulting.
- [GetSystemWow64DirectoryW](https://learn.microsoft.com/en-us/windows/win32/api/wow64apiset/nf-wow64apiset-getsystemwow64directoryw):
  "On 32-bit Windows, the function always fails, and the extended error is set
  to ERROR_CALL_NOT_IMPLEMENTED." Decision: the WOW64 entries are W21 stubs
  with that failure; the same failure is used for the Windows 9x-absent
  service, NT security-token and activation-context entries.
- Wine 10.0 links KERNELBASE, which Windows 98 lacks. Decision: a Wine-side
  patch takes each KERNELBASE import from its Windows 98 home DLL found in the
  pinned specs (KERNEL32, ADVAPI32, USER32, GDI32, VERSION, SHLWAPI); SHLWAPI
  functions that Wine moved into KERNELBASE are compiled from the upstream
  kernelbase units into the reused SHLWAPI, as Windows 98 SHLWAPI contained
  them. These retarget/compile-in patches are decided, not yet written.
- IMM32: the Wine build (102,400 SizeOfImage, twelve WIN32U system-call
  imports) is measured but not reused; the CiukiOS W21 placeholder (20,480)
  keeps the same no-IME behaviour and gains three W21 exports.
- MSVCRT: Wine's PE exports all 85 functions the reused DLLs import and the
  MSVCRT ABI (1,290 names, `_iob`/FILE layout, `_errno`, locale, SEH and C++
  EH, startup). The CiukiOS newlib port covers 72 of the 85 with 93,374 text
  bytes over an empty program (24 pages) but lacks 13 functions and every
  MSVCRT ABI element that Windows 98-era programs and installer-placed
  `msvcrt.dll` imports bind. Decision: the Wine 10.0 `msvcrt.dll` PE becomes
  the single builtin MSVCRT once qualified (614,400 SizeOfImage, 153 startup
  pages in the store model); the libc port would be a new CRT, not a port.
- 128 MiB: the sixteen reused DLLs total 15,486,976 SizeOfImage bytes, above
  the contract's 14 MiB installer budget (P2+P4) before any P2 DLL; SHELL32
  alone is 9,363,456, of which 8,667,136 are resources and 7,543,454 icons
  (PNG 256x256 2,748,974; 32-bpp 48x48 1,715,920; 32-bpp 32x32 758,992).
  Windows 98 uses at most 8-bpp 16/32/48 icons. This exceeds a contract
  target and returns to the lead; an icon-format subset of the SHELL32
  resources would remove about 6 MB but is not applied.

### F5-20D cache, staging and probe review (2026-10-20)

The cached build separates PE compilation from core import qualification.
Wine 10.0's [MSVCRT Makefile](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/msvcrt/Makefile.in)
and [COMBASE Makefile](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/combase/Makefile.in)
name upstream import libraries; the recipe does not link CiukiOS core PE
bytes. Consequently an unchanged archive/patch/recipe reuses the exact
recorded PEs when the core changes, then rechecks the actual exports,
forwarders and W21 decisions. Byte-only core changes refresh the record's
hashes automatically. The audit cache also checks actual core PE hashes and
coverage statuses, even if `builtin-store.json` has not been regenerated.
Changed decisions or missing exports fail before the record is written.

Qualification uses the existing H0 binding and H1 attach/detach evidence
for each exact PE hash; all sixteen reused DLLs meet that rule. IMM32 stays
unqualified. Only byte-identical generated census placeholders may be
replaced during image staging; another same-named builtin is refused.

[Microsoft's DllMain documentation](https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain)
defines attach on load and detach on unloading or process exit. CiukiOS
builtins remain resident after `FreeLibrary`; `ExitProcess` calls
`CiukiProcessDetach` and the production loader's shutdown. The fixture
therefore balances its loads, then exits normally so detach actually runs.
The host dry-run observes the attach/detach calls; the guest controller
waits for a clean process exit before publishing its result. Guest evidence
remains pending. Launch waits for the desktop's registry round trip, as in
the corrected f5-10C2 dialogs fixture.
The H1 adapter has a separate `sandbox-check` mode that returns before any
PE mapping. Only a refusal or SIGSYS in that preflight is an execution
blocker skip; SIGSYS during the actual DLL exercise remains a failure.
[Linux documents](https://docs.kernel.org/userspace-api/seccomp_filter.html)
that a seccomp kill action terminates with SIGSYS and that filters stack;
the adapter never retries PE execution without its own confinement filter.

The partial implementation's `GlobalAlloc` flag fix is retained:
[Microsoft documents](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-globalalloc)
the obsolete NODISCARD/LOWER/NOTIFY flags as accepted and ignored, and a
zero-byte movable allocation as a valid discarded handle. Pinned Wine
[COML2 memlockbytes.c](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/coml2/memlockbytes.c)
requests `GMEM_MOVEABLE | GMEM_NODISCARD`; refusing that valid flag prevented
the fixture's in-memory OLE storage exercise. Host tests cover allocation,
growth, locking, freeing and an invalid flag.

Wine's [LICENSE](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/LICENSE)
and [COPYING.LIB sections 4 and 6](https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/COPYING.LIB)
require the notices and corresponding source arrangement for distribution.
Keep the exact source archive, patches and complete recipe helpers available
with the release; upstream URLs and the local ignored archive alone do not
constitute that distribution. Publication remains suspended.
The bundled [musl copyright notice](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/musl/COPYRIGHT)
and [zlib header notice](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/zlib/zlib.h)
are copied from the verified local upstream sources after comparison with
`config/wine-pin.json` and staged separately as `WINE-musl-COPYRIGHT.txt`
and `WINE-zlib-NOTICE.txt` with MSVCRT and CABINET.

The tables below are generated from `config/wine-upper.json` by
`python3 scripts/win32/wine_upper.py render` and checked by
`python3 scripts/win32/wine_upper.py check` and
`tests/host/win32/wine/test_wine_upper.py`.

<!-- BEGIN GENERATED WINE UPPER -->

### Reused upper DLL builds (generated from `config/wine-upper.json`)

Wine `b073859675060c9211fcbccfd90e4e87520dc2c2`, archive SHA-256 `c5e0b3f5f7efafb30e9cd4d9c624b85c583171d33549d933cd3402f341ac3601`, 2 consecutive runs with identical hashes. CFLAGS `-O2 -g0 -march=pentiumpro -mno-sse -mno-sse2 -mno-mmx -fno-ident`, LDFLAGS `--no-default-config -Wl,-timestamp:0`, `llvm-objcopy --strip-all`.

| DLL | Base | File bytes | SizeOfImage | Seed pages | COW pages | Resource entries | SHA-256 (prefix) | Use |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| comctl32.dll | `0x74000000` | 1,175,552 | 1,179,648 | 287 | 3 | 440 | `2a53a09283331828` | reused, qualified (staged builtin) |
| comdlg32.dll | `0x74120000` | 970,752 | 970,752 | 237 | 2 | 1,213 | `88dc12a9b05acf40` | reused, qualified (staged builtin) |
| shell32.dll | `0x74210000` | 3,293,184 | 3,293,184 | 804 | 3 | 3,490 | `0b371c8d0ca931e5` | reused, qualified (staged builtin) |
| shlwapi.dll | `0x74540000` | 352,256 | 352,256 | 86 | 3 | 83 | `f0519aad6c7fca0a` | reused, qualified (staged builtin) |
| ole32.dll | `0x745a0000` | 630,784 | 630,784 | 154 | 2 | 70 | `af5a3bd5db02edd9` | reused, qualified (staged builtin) |
| combase.dll | `0x74640000` | 258,048 | 258,048 | 63 | 3 | 0 | `3dc27ea3f48ede46` | reused, qualified (staged builtin) |
| oleaut32.dll | `0x74680000` | 540,672 | 659,456 | 132 | 31 | 53 | `83d80452bdb4c017` | reused, qualified (staged builtin) |
| rpcrt4.dll | `0x74730000` | 425,984 | 425,984 | 104 | 3 | 3 | `49ef1848edb7a8b6` | reused, qualified (staged builtin) |
| riched20.dll | `0x747a0000` | 405,504 | 417,792 | 99 | 7 | 25 | `c29478a4fa6a9dbd` | reused, qualified (staged builtin) |
| riched32.dll | `0x74810000` | 32,768 | 32,768 | 8 | 2 | 3 | `f90eb4347ef58b55` | reused, qualified (staged builtin) |
| cabinet.dll | `0x74820000` | 106,496 | 106,496 | 26 | 2 | 3 | `64b049e00ddfeb18` | reused, qualified (staged builtin) |
| advpack.dll | `0x74840000` | 65,536 | 65,536 | 16 | 2 | 0 | `bcd9c814631ba327` | reused, qualified (staged builtin) |
| setupapi.dll | `0x74850000` | 331,776 | 331,776 | 81 | 4 | 127 | `7ca99e8f76aef0f9` | reused, qualified (staged builtin) |
| imm32.dll | `0x748b0000` | 102,400 | 102,400 | 25 | 2 | 3 | `a324dd082d0accc6` | measured, not reused |
| msvcrt.dll | `0x748d0000` | 606,208 | 614,400 | 148 | 5 | 3 | `8161d40c05af35d2` | reused, qualified (staged builtin) |
| coml2.dll | `0x74970000` | 86,016 | 86,016 | 21 | 2 | 0 | `a503cc5b92f4bd1e` | reused, qualified (staged builtin) |
| shcore.dll | `0x74990000` | 77,824 | 77,824 | 19 | 3 | 0 | `c0a9ea8c303f6f6e` | reused, qualified (staged builtin) |

| Patch (applied in order) | SHA-256 (prefix) |
| --- | --- |
| `0001-in-process-rpc-only.patch` | `579a606cb07ec8f9` |
| `0002-disable-rpcss-activation.patch` | `93a93e66fb2434bf` |
| `0003-setupdi-unsupported.patch` | `1c92e3ce44705942` |
| `0004-single-msvcrt-crt.patch` | `dbb16d028322a51c` |
| `0005-builtin-region-base.patch` | `f35d63a7a96108f6` |
| `0006-shell32-icon-trim.json` | `48157747e1c418e9` |
| `0007-kernelbase-retarget.patch` | `4de38f4475f23eea` |
| `0008-kernelbase-compile-in.patch` | `3cc7558345be2bdc` |
| `0009-shell32-wine-private-calls.patch` | `7ad67fb7f42f39af` |
| `0010-msvcrt-sbcs-code-page.patch` | `f8a8a9fd32068f11` |

`0006-shell32-icon-trim.json` (resource filter): 430 icon images (1,776,519 bytes) in, 235 (357,298 bytes) out.

### Import re-audit against the f5-20A/B core (generated)

Missing eager imports, delayed imports and unresolved export forwarders of the current build, and imports whose final hop is a core W21 stub (typed failure); each has a decision (`audit` = `f5-20c`, or its f5-14B row).

| DLL | Eager missing | Delayed missing | Unresolved forwarders | Bound to W21 stubs |
| --- | ---: | ---: | ---: | ---: |
| advpack.dll | 0 | 0 | 0 | 0 |
| cabinet.dll | 0 | 0 | 0 | 0 |
| combase.dll | 0 | 0 | 0 | 5 |
| comctl32.dll | 0 | 0 | 0 | 5 |
| comdlg32.dll | 0 | 0 | 0 | 11 |
| coml2.dll | 0 | 0 | 0 | 0 |
| imm32.dll | 38 | 0 | 0 | 0 |
| msvcrt.dll | 0 | 0 | 0 | 2 |
| ole32.dll | 0 | 0 | 0 | 1 |
| oleaut32.dll | 0 | 0 | 0 | 1 |
| riched20.dll | 0 | 0 | 0 | 3 |
| riched32.dll | 0 | 0 | 0 | 0 |
| rpcrt4.dll | 0 | 0 | 0 | 5 |
| setupapi.dll | 0 | 0 | 0 | 10 |
| shcore.dll | 0 | 0 | 0 | 0 |
| shell32.dll | 0 | 0 | 0 | 13 |
| shlwapi.dll | 0 | 0 | 3 | 2 |

| Audit | Action | Entries |
| --- | --- | ---: |
| f5-14b | `core-implement` | 759 |
| f5-14b | `core-w21-stub` | 61 |
| f5-14b | `not-reused` | 50 |
| f5-14b | `w21-stub-dll` | 57 |
| f5-14b | `w21-unresolved-forwarder` | 3 |
| f5-14b | `wine-compile-in` | 212 |
| f5-14b | `wine-patch-remove` | 3 |
| f5-14b | `wine-retarget` | 197 |
| f5-20b | `core-implement` | 218 |
| f5-20c | `core-implement` | 7 |
| f5-20c | `core-w21-stub` | 7 |
| f5-20c | `not-reused` | 38 |
| f5-20c | `w21-unresolved-forwarder` | 3 |

Closure of the f5-14B decisions in this build:

| Closure | Entries |
| --- | ---: |
| closed: CiukiOS W21 stub DLL (scripts/win32/upper_dlls.py ABSENT_DELAYED) | 57 |
| closed: core export (f5-20A/f5-20B) | 757 |
| closed: core typed W21 failure | 61 |
| closed: importer not reused | 50 |
| closed: lazy forwarder to an absent DLL (decided) | 3 |
| closed: no longer imported | 2 |
| closed: patch 0008: compiled into the importing DLL | 212 |
| closed: patch 0009: call removed | 3 |
| closed: patches 0007/0008: imported from the Windows 98 home DLL | 197 |

| Rule | Action | Decision |
| --- | --- | --- |
| `absent-delayed-dll` | `w21-stub-dll` | Delay-imported DLL absent from CiukiOS; a generated CiukiOS W21 stub DLL returns the documented failure per function (stub signatures), so delay binding succeeds and the caller sees failure (no theme, codec, trust provider or network). |
| `absent-forwarder-dll` | `w21-unresolved-forwarder` | Export forwarder into a DLL absent from CiukiOS; forwarders bind lazily, so the DLL loads and a binder of this export gets the W21 missing-export diagnostic. |
| `ciukios-stub-dll` | `core-w21-stub` | Provided by an existing CiukiOS census/W21 DLL that lacks the export; add a W21 stub with the documented failure (no printer, no IME, no network provider). |
| `ciukios-version` | `core-implement` | CiukiOS VERSION implements the ANSI functions; the Unicode functions are added (core amendment). |
| `core-api` | `core-implement` | Documented Win32 API reached by the reused DLL; the owning CiukiOS core DLL implements it (amendment of the owning core directive). |
| `core-api-f5-20b` | `core-implement` | Win32 export outside the f5-14B table: imported by a reused DLL but then exported by the core only as a census or Unicode W21 stub (so the audit did not report it missing), or the ANSI/Unicode partner of a decided name; implemented by f5-20B (MSLU model for the W forms; amendment 2 item 1). |
| `core-api-f5-20c` | `core-implement` | Import bound to a nonreturning KERNEL32 census stub when the f5-20C re-audit ran; implemented in f5-20C (lead decision): IsBadReadPtr/IsBadWritePtr/IsBadCodePtr probe the range through the virtual-memory query and never fault, DebugBreak raises EXCEPTION_BREAKPOINT (int 3) as Windows does. |
| `core-census-w21` | `core-w21-stub` | Import bound to an existing CiukiOS census or typed W21 stub (documented failure plus one W21 diagnostic); the reused DLL handles the failure. Recorded by the f5-20C re-audit (no Wine or core change). |
| `core-security-structures` | `core-implement` | Security descriptors and ACLs are plain data structures: the CiukiOS ADVAPI32 builds them in NT layout and object creation ignores them (no ACL enforcement on FAT); core amendment. |
| `core-uniscribe` | `core-implement` | Wine 10.0 moved Uniscribe (USP10) into GDI32 and RICHED20/COMCTL32 shape text through it; the CiukiOS GDI32 implements a left-to-right single-script subset (core amendment). |
| `kernelbase-internal` | `wine-compile-in` | KERNELBASE-only API without a Windows 98 home; Wine-side patch compiles the upstream kernelbase unit into the importing DLL. |
| `kernelbase-retarget` | `wine-retarget` | Wine 10.0 links KERNELBASE, which Windows 98 lacks; a Wine-side Makefile/spec patch takes the function from its Windows 98 home DLL (pinned Wine spec), where the CiukiOS core already exports it. |
| `kernelbase-shlwapi-compile-in` | `wine-compile-in` | SHLWAPI function that Wine 10.0 moved to KERNELBASE; Windows 98 SHLWAPI implemented it. Wine-side patch: compile the upstream kernelbase unit into the reused SHLWAPI. |
| `kernelbase-to-shlwapi` | `wine-retarget` | SHLWAPI function taken from KERNELBASE by another reused DLL; Wine-side patch retargets it to the reused SHLWAPI (after its compile-in). |
| `not-reused` | `not-reused` | The importing Wine build is measured but not reused; the entry disappears with it (see not_reused). |
| `ntdll-native` | `core-implement` | NT native API (Nt*/Rtl*/Ldr*) reached by the reused DLL; the CiukiOS NTDLL implements it over its own services (core amendment). |
| `ntdll-runtime` | `core-implement` | Compiler/CRT helper that NT NTDLL exports (64-bit arithmetic, stack probe, _vsnprintf); the CiukiOS NTDLL implements it (core amendment). |
| `w21-actctx` | `core-w21-stub` | Activation contexts are Windows XP+; W21 stub failing with ERROR_CALL_NOT_IMPLEMENTED, which the callers treat as no context. |
| `w21-dpi` | `core-w21-stub` | Per-monitor DPI internals (Windows 8.1+) reached only through SHCORE forwarders; W21 stub failing, callers fall back to 96 DPI. |
| `w21-msi` | `core-w21-stub` | Windows Installer descriptors (advertised shortcuts) need MSI, which CiukiOS refuses (W32_REFUSE_MSI); W21 stub returning ERROR_CALL_NOT_IMPLEMENTED. |
| `w21-nt-security` | `core-w21-stub` | NT token/ACL services (file ACLs, privileges, user tokens, SID strings) are absent from Windows 9x and FAT; Windows 9x fails with ERROR_CALL_NOT_IMPLEMENTED; W21 stub with that failure. |
| `w21-services` | `core-w21-stub` | Service Control Manager: Windows 9x has none and fails with ERROR_CALL_NOT_IMPLEMENTED; W21 diagnostic stub with that failure. |
| `w21-thread-description` | `core-w21-stub` | Thread descriptions are Windows 10 diagnostics; W21 stub returning E_NOTIMPL. |
| `w21-wow64` | `core-w21-stub` | WOW64 redirection exists only on 64-bit Windows; documented to fail on 32-bit Windows; W21 stub with ERROR_CALL_NOT_IMPLEMENTED. |
| `wine-private` | `wine-patch-remove` | Wine-private helper with no Windows 98 meaning; a Wine-side source patch removes the call path. No Wine-private export enters the core. |

Unique names per audit, rule and owning DLL:

- f5-14b `absent-delayed-dll` → gdiplus.dll (7): GdipCreateFromHDC, GdipDeleteGraphics, GdipDisposeImage, GdipDrawImageRectRectI, GdipGetImageDimension, GdipGetImageThumbnail, GdipLoadImageFromFile
- f5-14b `absent-delayed-dll` → iphlpapi.dll (1): GetAdaptersInfo
- f5-14b `absent-delayed-dll` → mlang.dll (1): ConvertINetUnicodeToMultiByte
- f5-14b `absent-delayed-dll` → secur32.dll (18): AcceptSecurityContext, AcquireCredentialsHandleA, AcquireCredentialsHandleW, CompleteAuthToken, DecryptMessage, DeleteSecurityContext, EncryptMessage, EnumerateSecurityPackagesA, EnumerateSecurityPackagesW, FreeContextBuffer, FreeCredentialsHandle, GetUserNameExW, ImpersonateSecurityContext, InitializeSecurityContextW, MakeSignature, QueryContextAttributesA, RevertSecurityContext, VerifySignature
- f5-14b `absent-delayed-dll` → shdocvw.dll (1): #218
- f5-14b `absent-delayed-dll` → urlmon.dll (4): CoInternetParseUrl, CreateFormatEnumerator, CreateURLMoniker, RegisterFormatEnumerator
- f5-14b `absent-delayed-dll` → uxtheme.dll (20): CloseThemeData, DrawThemeBackground, DrawThemeEdge, DrawThemeParentBackground, DrawThemeText, DrawThemeTextEx, EnableThemeDialogTexture, GetThemeBackgroundContentRect, GetThemeBackgroundExtent, GetThemeColor, GetThemeFont, GetThemeInt, GetThemeMargins, GetThemePartSize, GetThemeTextExtent, GetWindowTheme, IsThemeBackgroundPartiallyTransparent, IsThemePartDefined, OpenThemeData, OpenThemeDataForDpi
- f5-14b `absent-delayed-dll` → windowscodecs.dll (1): WICConvertBitmapSource
- f5-14b `absent-delayed-dll` → wintrust.dll (4): CryptCATAdminAcquireContext, CryptCATAdminAddCatalog, CryptCATAdminReleaseCatalogContext, CryptCATAdminReleaseContext
- f5-14b `absent-forwarder-dll` → userenv.dll (2): ExpandEnvironmentStringsForUserA, ExpandEnvironmentStringsForUserW
- f5-14b `absent-forwarder-dll` → wininet.dll (1): InternetCrackUrlW
- f5-14b `ciukios-stub-dll` → imm32.dll (3): ImmGetCompositionStringW, ImmSetCompositionFontW, ImmSetCompositionWindow
- f5-14b `ciukios-stub-dll` → mpr.dll (2): WNetGetLastErrorW, WNetRestoreConnectionW
- f5-14b `ciukios-stub-dll` → winspool.drv (12): DeviceCapabilitiesA, DeviceCapabilitiesW, DocumentPropertiesW, EnumPrintersA, EnumPrintersW, GetDefaultPrinterA, GetDefaultPrinterW, GetPrinterA, GetPrinterDriverA, GetPrinterDriverW, GetPrinterW, OpenPrinterW
- f5-14b `ciukios-version` → version.dll (3): GetFileVersionInfoSizeW, GetFileVersionInfoW, VerQueryValueW
- f5-14b `core-api` → advapi32.dll (24): GetCurrentHwProfileA, GetUserNameW, RegCreateKeyExW, RegCreateKeyW, RegDeleteKeyExW, RegDeleteKeyW, RegDeleteTreeA, RegDeleteTreeW, RegDeleteValueW, RegEnumKeyExW, RegEnumKeyW, RegEnumValueW, RegGetValueA, RegGetValueW, RegLoadMUIStringA, RegLoadMUIStringW, RegOpenKeyExW, RegOpenKeyW, RegQueryInfoKeyW, RegQueryValueExW, RegQueryValueW, RegSetValueExW, RegSetValueW, SystemFunction036
- f5-14b `core-api` → gdi32.dll (38): CloseMetaFile, CopyEnhMetaFileW, CopyMetaFileW, CreateColorSpaceW, CreateFontIndirectW, CreateFontW, CreateHalftonePalette, CreateMetaFileW, CreateRoundRectRgn, DeleteEnhMetaFile, EnumFontFamiliesExW, EnumFontFamiliesW, ExtCreatePen, ExtEscape, GdiAlphaBlend, GdiConvertToDevmodeW, GdiGetCharDimensions, GdiGetCodePage, GetBitmapBits, GetCharABCWidthsW, GetCharWidth32W, GetCharWidthInfo, GetCharWidthW, GetCharacterPlacementW, GetEnhMetaFileBits, GetEnhMetaFileHeader, GetLayout, GetMetaFileBitsEx, GetTextCharsetInfo, GetTextFaceW, GetTextMetricsW, GetWinMetaFileBits, PlayEnhMetaFile, PolyPolyline, SetEnhMetaFileBits, SetWinMetaFileBits, StartDocW, TranslateCharsetInfo
- f5-14b `core-api` → kernel32.dll (99): Beep, CompareStringOrdinal, ConvertDefaultLocale, CopyFileW, CreateDirectoryW, CreateFileMappingW, CreatePipe, CreateSemaphoreW, CreateTimerQueueTimer, DelayLoadFailureHook, DeleteFileW, DeviceIoControl, EnumResourceNamesW, EnumSystemLocalesEx, ExpandEnvironmentStringsA, ExpandEnvironmentStringsW, FindFirstFileW, FindNextFileW, FindResourceExW, FindResourceW, FormatMessageW, GetBinaryTypeW, GetCPInfoExW, GetConsoleMode, GetCurrencyFormatW, GetCurrentDirectoryW, GetDateFormatW, GetDiskFreeSpaceExW, GetDiskFreeSpaceW, GetDriveTypeW, GetFileAttributesExW, GetFileAttributesW, GetFileInformationByHandle, GetFileSizeEx, GetFinalPathNameByHandleW, GetFullPathNameW, GetLocaleInfoEx, GetLogicalDriveStringsW, GetLongPathNameW, GetNumberFormatW, GetPrivateProfileIntW, GetPrivateProfileStringW, GetPrivateProfileStructW, GetProfileStringW, GetShortPathNameW, GetStringTypeExW, GetSystemDefaultUILanguage, GetSystemDirectoryW, GetSystemWindowsDirectoryA, GetSystemWindowsDirectoryW, GetTempFileNameW, GetTempPathW, GetThreadId, GetTickCount64, GetTimeFormatW, GetUserDefaultLocaleName, GetUserDefaultUILanguage, GetVolumeInformationW, GetWindowsDirectoryW, GlobalAddAtomW, GlobalFindAtomW, HeapCompact, InitOnceExecuteOnce, InitializeCriticalSectionEx, IsBadStringPtrA, IsBadStringPtrW, IsValidLocaleName, IsWow64Process, LCIDToLocaleName, LZClose, LZCopy, LZOpenFileW, LZRead, LZSeek, LocaleNameToLCID, MoveFileExW, MoveFileW, OpenEventW, OpenSemaphoreW, OutputDebugStringW, PeekConsoleInputA, PostQueuedCompletionStatus, ProcessIdToSessionId, QueueUserWorkItem, ReadConsoleA, ReadConsoleInputW, RemoveDirectoryW, ResolveDelayLoadedAPI, SearchPathW, SetConsoleMode, SetCurrentDirectoryW, SetFileAttributesW, SetFilePointerEx, SystemTimeToTzSpecificLocalTime, VerifyConsoleIoHandle, WritePrivateProfileStringW, WritePrivateProfileStructW, lstrcmpW, lstrcmpiW
- f5-14b `core-api` → user32.dll (105): AppendMenuW, CharLowerBuffW, CharLowerW, CharNextW, CharPrevW, CharToOemW, CharUpperBuffW, CharUpperW, CheckMenuRadioItem, ChildWindowFromPoint, CopyAcceleratorTableW, CopyIcon, CopyImage, CreateAcceleratorTableW, CreateCaret, CreateDialogIndirectParamW, CreateDialogParamW, CreateIconFromResourceEx, CreatePopupMenu, DdeCreateStringHandleW, DdeGetLastError, DdeInitializeW, DdeQueryStringW, DialogBoxIndirectParamW, DialogBoxParamW, DlgDirListComboBoxW, DlgDirListW, DragDetect, DrawFrameControl, DrawStateW, DrawTextExW, DrawTextW, EnableScrollBar, EnumClipboardFormats, GetAncestor, GetCaretPos, GetClipboardFormatNameA, GetClipboardFormatNameW, GetClipboardSequenceNumber, GetDlgItemInt, GetDlgItemTextW, GetDpiForWindow, GetIconInfo, GetMenuDefaultItem, GetMenuInfo, GetMenuItemInfoW, GetMenuItemRect, GetMenuStringW, GetMonitorInfoW, GetNextDlgGroupItem, GetScrollInfo, GetScrollPos, GetWindowInfo, GetWindowRgn, InsertMenuItemW, InsertMenuW, InternalGetWindowText, InvertRect, IsCharAlphaNumericW, IsCharAlphaW, IsCharLowerW, IsCharUpperW, IsDialogMessageW, IsDlgButtonChecked, IsProcessDPIAware, KillSystemTimer, LoadAcceleratorsW, LoadBitmapW, LoadImageW, LoadMenuW, LoadStringW, MapDialogRect, MessageBoxIndirectW, MessageBoxW, ModifyMenuW, MonitorFromRect, MonitorFromWindow, NotifyWinEvent, OemToCharW, PrivateExtractIconExW, PrivateExtractIconsW, RegisterClipboardFormatA, RegisterClipboardFormatW, RemoveMenu, SendDlgItemMessageW, SetCaretPos, SetDlgItemInt, SetDlgItemTextW, SetMenuDefaultItem, SetMenuInfo, SetMenuItemInfoW, SetScrollInfo, SetScrollRange, SetSystemTimer, SetWindowRgn, ShowScrollBar, SubtractRect, TabbedTextOutW, TrackMouseEvent, TrackPopupMenu, TrackPopupMenuEx, TranslateAcceleratorW, WinHelpW, wsprintfW, wvsprintfW
- f5-14b `core-security-structures` → advapi32.dll (9): AddAccessAllowedAce, AddAccessAllowedAceEx, AddAccessDeniedAce, AddAccessDeniedAceEx, GetLengthSid, InitializeAcl, InitializeSecurityDescriptor, SetSecurityDescriptorDacl, SetSecurityDescriptorOwner
- f5-14b `core-uniscribe` → gdi32.dll (17): ScriptBreak, ScriptCPtoX, ScriptFreeCache, ScriptItemize, ScriptLayout, ScriptPlace, ScriptShape, ScriptStringAnalyse, ScriptStringCPtoX, ScriptStringFree, ScriptStringGetLogicalWidths, ScriptStringOut, ScriptStringXtoCP, ScriptString_pSize, ScriptString_pcOutChars, ScriptTextOut, ScriptXtoCP
- f5-14b `kernelbase-internal` → ole32.dll (1): PathCchFindExtension
- f5-14b `kernelbase-retarget` → kernel32.dll (85): CloseHandle, CompareFileTime, CompareStringA, CompareStringW, CreateEventW, CreateFileA, CreateFileW, CreateProcessW, CreateThread, DeleteCriticalSection, DisableThreadLibraryCalls, DuplicateHandle, EnterCriticalSection, FileTimeToSystemTime, FindClose, FreeLibrary, FreeResource, GetACP, GetEnvironmentVariableW, GetFileAttributesA, GetFileSize, GetFullPathNameA, GetLocalTime, GetLocaleInfoW, GetModuleFileNameA, GetModuleFileNameW, GetModuleHandleA, GetModuleHandleW, GetProcAddress, GetStringTypeW, GetSystemInfo, GetThreadLocale, GetTickCount, GetUserDefaultLCID, GetVersion, GetVersionExW, GlobalAlloc, GlobalFree, HeapAlloc, HeapFree, HeapReAlloc, HeapSize, IsDBCSLeadByte, IsValidLocale, LeaveCriticalSection, LoadLibraryA, LoadLibraryExA, LoadLibraryExW, LoadLibraryW, LoadResource, LocalAlloc, LocalFree, LocalLock, LocalReAlloc, LocalUnlock, LockFileEx, LockResource, MapViewOfFile, MulDiv, MultiByteToWideChar, OpenProcess, QueryPerformanceCounter, QueryPerformanceFrequency, RaiseException, ReadFile, ReleaseSemaphore, ResetEvent, SetEndOfFile, SetErrorMode, SetEvent, SetFileAttributesA, SetFilePointer, SizeofResource, Sleep, SystemTimeToFileTime, TerminateProcess, UnlockFileEx, UnmapViewOfFile, VirtualFree, VirtualQuery, WaitForSingleObject, WideCharToMultiByte, WriteFile, lstrcmpA, lstrcmpiA
- f5-14b `kernelbase-shlwapi-compile-in` → shlwapi.dll (211): ChrCmpIA, ChrCmpIW, GetAcceptLanguagesA, GetAcceptLanguagesW, HashData, IsCharBlankW, IsCharCntrlW, IsCharDigitW, IsCharPunctW, IsCharSpaceA, IsCharSpaceW, IsCharXDigitW, IsInternetESCEnabled, ParseURLA, ParseURLW, PathAddBackslashA, PathAddBackslashW, PathAddExtensionA, PathAddExtensionW, PathAppendA, PathAppendW, PathCanonicalizeA, PathCanonicalizeW, PathCombineA, PathCombineW, PathCommonPrefixA, PathCommonPrefixW, PathCreateFromUrlA, PathCreateFromUrlAlloc, PathCreateFromUrlW, PathFileExistsA, PathFileExistsW, PathFindExtensionA, PathFindExtensionW, PathFindFileNameA, PathFindFileNameW, PathFindNextComponentA, PathFindNextComponentW, PathGetArgsA, PathGetArgsW, PathGetCharTypeA, PathGetCharTypeW, PathGetDriveNumberA, PathGetDriveNumberW, PathIsFileSpecA, PathIsFileSpecW, PathIsLFNFileSpecA, PathIsLFNFileSpecW, PathIsPrefixA, PathIsPrefixW, PathIsRelativeA, PathIsRelativeW, PathIsRootA, PathIsRootW, PathIsSameRootA, PathIsSameRootW, PathIsUNCA, PathIsUNCServerA, PathIsUNCServerShareA, PathIsUNCServerShareW, PathIsUNCServerW, PathIsUNCW, PathIsURLA, PathIsURLW, PathIsValidCharA, PathIsValidCharW, PathMatchSpecA, PathMatchSpecExA, PathMatchSpecExW, PathMatchSpecW, PathParseIconLocationA, PathParseIconLocationW, PathQuoteSpacesA, PathQuoteSpacesW, PathRelativePathToA, PathRelativePathToW, PathRemoveBackslashA, PathRemoveBackslashW, PathRemoveBlanksA, PathRemoveBlanksW, PathRemoveExtensionA, PathRemoveExtensionW, PathRemoveFileSpecA, PathRemoveFileSpecW, PathRenameExtensionA, PathRenameExtensionW, PathSearchAndQualifyA, PathSearchAndQualifyW, PathSkipRootA, PathSkipRootW, PathStripPathA, PathStripPathW, PathStripToRootA, PathStripToRootW, PathUnExpandEnvStringsA, PathUnExpandEnvStringsW, PathUnquoteSpacesA, PathUnquoteSpacesW, QISearch, SHLoadIndirectString, SHRegCloseUSKey, SHRegCreateUSKeyA, SHRegCreateUSKeyW, SHRegDeleteEmptyUSKeyA, SHRegDeleteEmptyUSKeyW, SHRegDeleteUSValueA, SHRegDeleteUSValueW, SHRegEnumUSKeyA, SHRegEnumUSKeyW, SHRegEnumUSValueA, SHRegEnumUSValueW, SHRegGetBoolUSValueA, SHRegGetBoolUSValueW, SHRegGetUSValueA, SHRegGetUSValueW, SHRegOpenUSKeyA, SHRegOpenUSKeyW, SHRegQueryInfoUSKeyA, SHRegQueryInfoUSKeyW, SHRegQueryUSValueA, SHRegQueryUSValueW, SHRegSetUSValueA, SHRegSetUSValueW, SHRegWriteUSValueA, SHRegWriteUSValueW, SHTruncateString, StrCSpnA, StrCSpnIA, StrCSpnIW, StrCSpnW, StrCatBuffA, StrCatBuffW, StrCatChainW, StrChrA, StrChrIA, StrChrIW, StrChrNW, StrChrW, StrCmpCA, StrCmpCW, StrCmpICA, StrCmpICW, StrCmpIW, StrCmpLogicalW, StrCmpNA, StrCmpNCA, StrCmpNCW, StrCmpNIA, StrCmpNICA, StrCmpNICW, StrCmpNIW, StrCmpNW, StrCmpW, StrCpyNW, StrCpyNXA, StrCpyNXW, StrDupA, StrDupW, StrIsIntlEqualA, StrIsIntlEqualW, StrPBrkA, StrPBrkW, StrRChrA, StrRChrIA, StrRChrIW, StrRChrW, StrRStrIA, StrRStrIW, StrSpnA, StrSpnW, StrStrA, StrStrIA, StrStrIW, StrStrNIW, StrStrNW, StrStrW, StrToInt64ExA, StrToInt64ExW, StrToIntA, StrToIntExA, StrToIntExW, StrToIntW, StrTrimA, StrTrimW, UrlApplySchemeA, UrlApplySchemeW, UrlCanonicalizeA, UrlCanonicalizeW, UrlCombineA, UrlCombineW, UrlCompareA, UrlCompareW, UrlCreateFromPathA, UrlCreateFromPathW, UrlEscapeA, UrlEscapeW, UrlFixupW, UrlGetLocationA, UrlGetLocationW, UrlGetPartA, UrlGetPartW, UrlHashA, UrlHashW, UrlIsA, UrlIsNoHistoryA, UrlIsNoHistoryW, UrlIsOpaqueA, UrlIsOpaqueW, UrlIsW, UrlUnescapeA, UrlUnescapeW
- f5-14b `kernelbase-to-shlwapi` → shlwapi.dll (27): PathAddExtensionW, PathRemoveExtensionW, StrCSpnA, StrCSpnIA, StrCSpnIW, StrCSpnW, StrChrA, StrChrIA, StrChrIW, StrChrW, StrCmpIW, StrCmpNA, StrCmpNIA, StrCmpNIW, StrCmpNW, StrRChrA, StrRChrIA, StrRChrIW, StrRChrW, StrRStrIA, StrRStrIW, StrStrA, StrStrIA, StrStrIW, StrStrW, StrToIntA, StrToIntW
- f5-14b `not-reused` → imm32.dll (50): CreateFontIndirectW, DelayLoadFailureHook, DrawTextW, EnterCriticalSection, FreeLibrary, GetFileVersionInfoSizeW, GetFileVersionInfoW, GetModuleHandleA, GetModuleHandleW, GetMonitorInfoW, GetProcAddress, GetTickCount, GlobalAlloc, GlobalFree, HeapAlloc, HeapReAlloc, InitOnceExecuteOnce, IsBadStringPtrA, IsBadStringPtrW, IsDBCSLeadByte, LCMapStringW, LeaveCriticalSection, LoadLibraryW, MonitorFromPoint, MonitorFromWindow, MultiByteToWideChar, NtUserAssociateInputContext, NtUserBuildHimcList, NtUserCallHwnd, NtUserCallTwoParam, NtUserCreateInputContext, NtUserDestroyInputContext, NtUserDisableThreadIme, NtUserMessageCall, NtUserNotifyIMEStatus, NtUserQueryInputContext, NtUserQueryWindow, NtUserUpdateInputContext, RaiseException, RegCreateKeyExW, RegDeleteKeyW, RegGetValueW, RegOpenKeyW, RegSetValueExW, ResolveDelayLoadedAPI, RtlSetLastWin32Error, ToUnicodeEx, User32InitializeImmEntryTable, VerQueryValueW, WideCharToMultiByte
- f5-14b `ntdll-native` → ntdll.dll (20): LdrAddRefDll, NtAllocateVirtualMemory, NtClose, NtCreateKey, NtDeleteKey, NtOpenKey, NtQueryInformationFile, NtQuerySystemTime, RtlAreBitsClear, RtlAreBitsSet, RtlCreateUnicodeStringFromAsciiz, RtlGUIDFromString, RtlImageRvaToVa, RtlInitializeBitMap, RtlIsTextUnicode, RtlRandom, RtlSetBits, RtlTimeToSecondsSince1970, RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteSize
- f5-14b `ntdll-runtime` → ntdll.dll (4): _alldiv, _allrem, _aulldiv, _chkstk
- f5-14b `w21-actctx` → kernel32.dll (7): ActivateActCtx, CreateActCtxW, DeactivateActCtx, FindActCtxSectionGuid, FindActCtxSectionStringW, GetCurrentActCtx, ReleaseActCtx
- f5-14b `w21-dpi` → user32.dll (3): GetDpiForMonitorInternal, GetProcessDpiAwarenessInternal, SetProcessDpiAwarenessInternal
- f5-14b `w21-msi` → advapi32.dll (1): CommandLineFromMsiDescriptor
- f5-14b `w21-nt-security` → advapi32.dll (5): ConvertSidToStringSidW, CreateProcessAsUserW, GetFileSecurityW, LookupPrivilegeValueW, SetFileSecurityW
- f5-14b `w21-services` → advapi32.dll (8): ChangeServiceConfig2W, ChangeServiceConfigW, CreateServiceW, OpenSCManagerW, OpenServiceW, QueryServiceConfigW, QueryServiceStatusEx, StartServiceW
- f5-14b `w21-thread-description` → kernel32.dll (1): SetThreadDescription
- f5-14b `w21-wow64` → kernel32.dll (3): GetSystemWow64DirectoryW, Wow64DisableWow64FsRedirection, Wow64RevertWow64FsRedirection
- f5-14b `wine-private` → shell32.dll (3): wine_get_dos_file_name, wine_get_host_version, wine_get_unix_file_name
- f5-20b `core-api-f5-20b` → gdi32.dll (4): CopyEnhMetaFileA, CopyMetaFileA, CreateMetaFileA, DeleteColorSpace
- f5-20b `core-api-f5-20b` → user32.dll (87): BeginDeferWindowPos, CallMsgFilterW, CallWindowProcW, CharLowerA, CharLowerBuffA, CharNextA, CharPrevA, CharToOemA, CharToOemBuffA, CharToOemBuffW, CharUpperA, CharUpperBuffA, ChildWindowFromPointEx, CreateIconFromResource, CreateWindowExW, DdeClientTransaction, DdeConnect, DdeCreateDataHandle, DdeCreateStringHandleA, DdeDisconnect, DdeFreeDataHandle, DdeFreeStringHandle, DdeGetData, DdeInitializeA, DdeNameService, DdeQueryStringA, DdeUninitialize, DefDlgProcW, DefWindowProcW, DeferWindowPos, DispatchMessageW, DlgDirListA, DlgDirListComboBoxA, EndDeferWindowPos, FindWindowExW, FindWindowW, GetClassInfoExW, GetClassInfoW, GetClassLongW, GetClassNameW, GetDesktopWindow, GetMessageW, GetMonitorInfoA, GetPropW, GetTabbedTextExtentW, GetWindowLongW, GetWindowTextLengthW, GetWindowTextW, GrayStringW, IsCharAlphaA, IsCharAlphaNumericA, IsCharLowerA, IsCharUpperA, LoadCursorW, LoadIconW, LoadMenuIndirectW, MessageBoxExW, MessageBoxIndirectA, MonitorFromPoint, OemToCharA, OemToCharBuffA, OemToCharBuffW, PeekMessageW, PostMessageW, PostThreadMessageW, PrivateExtractIconExA, PrivateExtractIconsA, RegisterClassExW, RegisterClassW, RegisterWindowMessageW, RemovePropW, SendMessageTimeoutW, SendMessageW, SendNotifyMessageW, SetClassLongW, SetPropW, SetWindowLongW, SetWindowTextW, SetWindowsHookExW, SystemParametersInfoW, UnregisterClassW, VkKeyScanExW, VkKeyScanW, WaitForInputIdle, WinHelpA, wsprintfA, wvsprintfA
- f5-20c `absent-forwarder-dll` → userenv.dll (2): ExpandEnvironmentStringsForUserA, ExpandEnvironmentStringsForUserW
- f5-20c `absent-forwarder-dll` → wininet.dll (1): InternetCrackUrlW
- f5-20c `core-api-f5-20c` → kernel32.dll (4): DebugBreak, IsBadCodePtr, IsBadReadPtr, IsBadWritePtr
- f5-20c `core-census-w21` → imm32.dll (2): ImmGetContext, ImmReleaseContext
- f5-20c `core-census-w21` → kernel32.dll (2): GetNumberOfConsoleInputEvents, ReadConsoleInputA
- f5-20c `core-census-w21` → winspool.drv (1): DocumentPropertiesA
- f5-20c `not-reused` → imm32.dll (38): DelayLoadFailureHook, EnterCriticalSection, FreeLibrary, GetFileVersionInfoSizeW, GetFileVersionInfoW, GetModuleHandleA, GetModuleHandleW, GetProcAddress, GetTickCount, GlobalAlloc, GlobalFree, HeapAlloc, HeapReAlloc, InitOnceExecuteOnce, IsDBCSLeadByte, LCMapStringW, LeaveCriticalSection, LoadLibraryW, MultiByteToWideChar, NtUserAssociateInputContext, NtUserBuildHimcList, NtUserCallHwnd, NtUserCallTwoParam, NtUserCreateInputContext, NtUserDestroyInputContext, NtUserDisableThreadIme, NtUserMessageCall, NtUserNotifyIMEStatus, NtUserQueryInputContext, NtUserQueryWindow, NtUserUpdateInputContext, RaiseException, ResolveDelayLoadedAPI, RtlSetLastWin32Error, ToUnicodeEx, User32InitializeImmEntryTable, VerQueryValueW, WideCharToMultiByte

### Load and DllMain evidence (generated)

H0: native production-loader model, DllMain observed. H1: i386 Linux adapter, production loader and core PEs, every DllMain executed; PE ntdll with the H1 transport (identity, w32srv atoms, console), seccomp-confined.

| DLL | H0 binding | H0 DllMain attach/detach (observed) | H1 outcome | H1 DllMain attach TRUE | H1 detach | Kernel records ENOSYS | w32srv requests |
| --- | --- | ---: | --- | ---: | ---: | ---: | ---: |
| advpack.dll | bound | 12/12 | loaded | 11/11 | 11 | 10 | 0 |
| cabinet.dll | bound | 3/3 | loaded | 2/2 | 2 | 8 | 0 |
| combase.dll | bound | 8/8 | loaded | 7/7 | 7 | 8 | 0 |
| comctl32.dll | bound | 7/7 | loaded | 6/6 | 6 | 8 | 44 |
| comdlg32.dll | bound | 9/9 | loaded | 8/8 | 8 | 8 | 44 |
| coml2.dll | bound | 3/3 | loaded | 2/2 | 2 | 8 | 0 |
| msvcrt.dll | bound | 2/2 | loaded | 1/1 | 1 | 8 | 0 |
| ole32.dll | bound | 8/8 | loaded | 7/7 | 7 | 8 | 0 |
| oleaut32.dll | bound | 9/9 | loaded | 8/8 | 8 | 8 | 0 |
| riched20.dll | bound | 10/10 | loaded | 9/9 | 9 | 8 | 8 |
| riched32.dll | bound | 11/11 | loaded | 10/10 | 10 | 8 | 10 |
| rpcrt4.dll | bound | 3/3 | loaded | 2/2 | 2 | 8 | 0 |
| setupapi.dll | bound | 7/7 | loaded | 6/6 | 6 | 10 | 0 |
| shcore.dll | bound | 3/3 | loaded | 2/2 | 2 | 8 | 0 |
| shell32.dll | bound | 7/7 | loaded | 6/6 | 6 | 8 | 0 |
| shlwapi.dll | bound | 6/6 | loaded | 5/5 | 5 | 8 | 0 |

### Upstream conformance units on the H1 adapter (generated)

Wine 10.0 `dlls/*/tests` programs (test-runtime imports missing from the core are stubbed inside the program, listed below); the adapter has no CiukiOS kernel, so kernel record operations it does not model fail with ENOSYS.

| Unit | Outcome | Executed | Failures | Skipped | Failed checks reported | ENOSYS kernel operations |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| comctl32:imagelist | crashed c0000005 | - | - | - | 119 | 0x00000002×1, 0x00000006×2, 0x00000100×1, 0x0000010a×1 |
| comdlg32:filedlg | crashed c0000005 | - | - | - | 0 | 0x00000002×1, 0x00000006×2, 0x00000100×1, 0x00000109×18, 0x0000010a×1 |
| comdlg32:finddlg | completed | 43 | 18 | 0 | 18 | 0x00000002×1, 0x00000006×2, 0x00000100×3, 0x0000010a×1 |
| comdlg32:printdlg | completed | 15 | 0 | 4 | 0 | 0x00000002×2, 0x00000006×2, 0x00000100×3, 0x00000109×1, 0x0000010a×1 |
| ole32:compobj | crashed c0000005 | - | - | - | 25 | 0x00000002×1, 0x00000006×15, 0x0000000a×2, 0x00000100×4, 0x00000101×2, 0x00000109×9, 0x0000010a×1 |
| ole32:ole2 | crashed c0000005 | - | - | - | 1 | 0x00000002×2, 0x00000006×2, 0x00000100×1, 0x00000109×1, 0x0000010a×1 |
| riched20:txtsrv | completed | 213 | 13 | 0 | 13 | 0x00000002×1, 0x00000006×5, 0x00000100×3, 0x00000109×27, 0x0000010a×1 |
| shell32:brsfolder | completed | 1 | 1 | 4 | 1 | 0x00000002×1, 0x00000006×3, 0x00000100×3, 0x00000109×4, 0x0000010a×1 |
| shell32:shelllink | crashed c0000005 | - | - | - | 22 | 0x00000002×1, 0x00000006×2, 0x00000100×3, 0x00000109×15, 0x0000010a×1 |
| shell32:shellole | crashed c0000005 | - | - | - | 20 | 0x00000002×1, 0x00000006×3, 0x0000000a×2, 0x00000100×7, 0x00000101×4, 0x0000010a×1 |
| shell32:shellpath | completed | 0 | 0 | 0 | 0 | 0x00000002×1, 0x00000006×2, 0x00000100×3, 0x00000109×1, 0x0000010a×1 |
| shell32:shlexec | exited | - | - | - | 2 | 0x00000002×1, 0x00000006×4, 0x00000100×4, 0x00000109×6, 0x0000010a×1 |
| shell32:shlfileop | crashed c0000005 | - | - | - | 325 | 0x00000002×1, 0x00000006×4, 0x00000100×201, 0x00000109×1864, 0x0000010a×1 |
| shell32:shlfolder | crashed c0000005 | - | - | - | 0 | 0x00000002×1, 0x00000006×3, 0x00000100×1, 0x0000010a×1 |
| shell32:systray | timeout | - | - | - | 7 | - |

Test-runtime stubs per program: comctl32: gdi32.dll!CloseEnhMetaFile, gdi32.dll!CreateEnhMetaFileW, imm32.dll!ImmSetCompositionStringA, kernel32.dll!CreateActCtxA, kernel32.dll!FindActCtxSectionStringA, kernel32.dll!GetProcessId, user32.dll!CreateIcon, user32.dll!DlgDirSelectExA, user32.dll!GetComboBoxInfo, user32.dll!GetListBoxInfo, user32.dll!GetMenuBarInfo, user32.dll!GetOpenClipboardWindow, user32.dll!RealGetWindowClassA, user32.dll!SendInput, user32.dll!SetProcessDefaultLayout, user32.dll!SetWinEventHook, user32.dll!SetWindowsHookA, user32.dll!UnhookWinEvent, user32.dll!UnhookWindowsHook, uxtheme.dll!SetWindowTheme; comdlg32: kernel32.dll!GetProcessId; ole32: gdi32.dll!CloseEnhMetaFile, gdi32.dll!CreateEnhMetaFileA, gdi32.dll!CreateEnhMetaFileW, kernel32.dll!GetProcessId, user32.dll!CreateIcon; riched20: kernel32.dll!GetProcessId, user32.dll!GetOpenClipboardWindow; shell32: kernel32.dll!GetProcessId, kernel32.dll!OpenFileMappingA, user32.dll!DdeCmpStringHandles, user32.dll!keybd_event

### MSVCRT measurement (generated)

Surface: the 87 MSVCRT functions the reused DLLs import.

| Candidate | Bytes | Pages (model) | Surface covered | Notes |
| --- | ---: | ---: | ---: | --- |
| Wine 10.0 `msvcrt.dll` PE | 606,208 file, 614,400 SizeOfImage | 153 (148 seed + 5 COW) | 87/87 | 1290 exported names, MSVCRT ABI |
| CiukiOS newlib port (SDK `libc.a` `cd8d9c82981bc3dd`) | 93,542 text + 420 data | 24 | 74/87 | missing: __iob_func, _assert, _create_locale, _findclose, _free_locale, _snwprintf_l, _wcslwr, _wfindfirst, _wfindnext, _wfopen, _wgetenv, _wopen, wcscpy_s; no MSVCRT ABI |

Wine side: actual PE and section-store model. Newlib side: static CiukiOS SDK ELF with every covered surface function referenced, --gc-sections, minus an empty program; MSVCRT ABI glue (FILE/_iob layout, _errno, locale, SEH/C++ EH, startup) not included.

### 128 MiB fit figures (generated)

| Figure | f5-20C build | f5-14B build |
| --- | ---: | ---: |
| Reused DLLs | 16 | 16 |
| File bytes | 9,359,360 | 15,343,616 |
| SizeOfImage | 9,502,720 | 15,486,976 |
| Shared seed pages | 2285 (9,359,360 bytes) | 3746 |
| Private COW commitment pages | 77 | 77 |
| Startup committed pages (model) | 2362 | 3823 |
| Contract installer budget (P2+P4) | 14,680,064 | 14,680,064 |
| SizeOfImage within budget | yes | no |
| SHELL32 SizeOfImage | 3,293,184 | 9,363,456 |

Cold section-store header/raw seed page union; writable COW commitment promises counted separately, not resident pages. Excludes DllMain heaps, page tables, metadata and other modules; not guest RSS or a whole-system 128 MiB fit result.

<!-- END GENERATED WINE UPPER -->

## Milestone C research and implementation boundary (2026-10-19)

All Wine references below refer to commit
`b073859675060c9211fcbccfd90e4e87520dc2c2`, never installed Wine headers.
The archive is still unavailable (sandbox DNS failure); individual tagged
files were read through the GitHub connector. The eight requested modules
are msacm32, imaadp32.acm, msadp32.acm, msvfw32, avifil32, msrle32,
msvidc32 and iccvid. The last two missing audit roots, avifil32 and iccvid,
must not be mistaken for modules measured by milestone A.

Primary sources and decisions, recorded before implementation:

- [IMA recommended practices, revision 3](https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf):
  use the integer step/index algorithm, saturation and little-endian WAVE
  block predictors; stereo payload groups contain four bytes per channel.
- [Microsoft RIFF multimedia update](https://www-mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/RIFFNEW.pdf)
  and the pinned [IMA](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/imaadp32.acm/imaadp32.c)
  / [MS ADPCM](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/msadp32.acm/msadp32.c)
  drivers: validate block headers, coefficient tables and samples per block;
  preserve predictor arithmetic and channel order. XAudio2's extended ADPCM
  is a different format and is not the WAVE_FORMAT_ADPCM target.
- [Microsoft bitmap compression](https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-compression):
  implement RLE4/RLE8 encoded runs, word-padded absolute runs, EOL/EOB and
  delta coordinates with explicit source and destination bounds.
- Pinned [Video 1 decoder by Mike Melanson](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/msvidc32/msvideo1.c)
  and [Cinepak decoder by Tim Ferguson](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/iccvid/iccvid.c):
  preserve block order, palette/RGB555 values, codebook inheritance and
  frame skipping. Bound every chunk and index; retain codebooks per handle,
  rather than using Wine's file-global input cursor.
- [acmFormatSuggest](https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmformatsuggest),
  [acmStreamSize](https://learn.microsoft.com/en-us/windows/win32/api/msacm/nf-msacm-acmstreamsize),
  pinned msacm32 stream/format sources, and
  [ICDecompress](https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-icdecompress):
  distinguish public ACM handles from driver messages; preserve i386
  stdcall/cdecl and the packed WAVE/ACM layouts. Unsupported operations
  return errors, never success without an operation.
- The pinned avifil32 Makefile imports msacm32, msvfw32, winmm, ole32,
  user32, advapi32 and rpcrt4. The existing default Wine PE recipe adds
  ucrtbase/kernel32/ntdll, and milestone A's version.dll demonstrates
  kernelbase and Wine-private debug dependencies. These dependencies are
  not made available by importing a Unix Wine runtime into CiukiOS.
  Bounded original implementations avoid this backend dependency; their
  resulting size, imports and coverage are measured separately. No Wine
  PE sizes or successful Wine runtime loads are inferred from source scans.

The original-game census has no supplied media or FourCC traces; the
research union imports ICInfo/MCIWndCreateA/MCIWndRegisterClass from
msvfw32, but does not establish an acceptance game's compressed format.
MCI window/playback belongs to f5-12B and remains distinct from decoding.

## Research and method decisions

- Select Wine **10.0**, the stable 10.x release, rather than a development
  10.x snapshot or the installed 11.18 tools. The [upstream announcement](https://list.winehq.org/hyperkitty/list/wine-announce%40list.winehq.org/thread/JN2HC7JA7DUGMRJBAD7RPYLE65K3MAFQ/)
  names the archive. The [tag refs](https://api.github.com/repos/wine-mirror/wine/git/matching-refs/tags/wine-10.0)
  contain 10.0 and its release candidates, with no later 10.0.y tag at audit time.
  Its [annotated tag](https://api.github.com/repos/wine-mirror/wine/git/tags/2e9cb99369e5ead9d7fe9836dcd43c05e42890bc)
  resolves to commit `b073859675060c9211fcbccfd90e4e87520dc2c2`.
- The archive SHA-256 is the independently signature-checked value recorded by
  the [Buildroot maintainer's update](https://lists.buildroot.org/pipermail/buildroot/2025-January/771333.html).
  Sandbox `curl` failed with exit 6, `Could not resolve host: dl.winehq.org`.
  The archive therefore remains **pending-download**, not locally verified.
  Sources were obtained through the GitHub connector at the tagged commit;
  do not treat those individual files as the verified release archive.
- Record [Wine 9.0](https://list.winehq.org/hyperkitty/list/wine-announce%40list.winehq.org/message/TB7WYKWZF4DWNQLWN3XZM2NLEAF6MKST/)
  as the alternative requested as "9.0 LTS" in the directive. Upstream calls
  it a stable release; that announcement does not promise LTS maintenance.
  Changing releases requires repeating the audit, not substituting this pin.
- Read the pinned [makedep defaults](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/makedep.c):
  `get_default_imports` adds static `winecrt0`, a CRT (normally **ucrtbase**
  for DLLs), `kernel32`, and `ntdll`. Explicit Makefile `IMPORTS` alone is
  not a complete dependency census; static GUID libraries are not PE DLLs.
- The pinned [Wine configure](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/configure.ac)
  supports clang/lld targeting `i686-windows`, with Wine's own `include/msvcrt`
  headers. Lack of MinGW GCC does not by itself prevent a Wine PE build.
  For external PE libraries, separately supplied target headers/libraries
  still matter. [MinGW-w64 12.0.0](https://www.mingw-w64.org/changelog/)
  defaults to UCRT; headers and CRT settings must agree if that route is used.
- In pinned [winegcc](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/tools/winegcc/winegcc.c),
  the Windows target selects PE; `-mno-cygwin` selects Microsoft CRT handling,
  and `--wine-builtin` post-processes the PE with winebuild `--builtin`.
  Neither flag alone converts an ELF Winelib module into a usable PE DLL.
  Inspect the resulting headers/imports and retain a SHA-256 manifest.

The source census will distinguish source references, `.spec` forwards,
runtime lookup strings, and actual linked PE imports. Source token matching
cannot prove preprocessor reachability, provider selection, IDL-generated
imports, or a complete runtime closure. An unbuilt DLL must not be described
as a measured import set or as ABI-freeze evidence.

<!-- BEGIN GENERATED WINE AUDIT -->

## Reproducible source-reference tables

These counts and lists are i386 source-reference candidates, not exact PE
imports. They include conditional/unreachable source paths and exclude
unexpanded IDL/header-generated code. Private/header helpers are additional
requirements; zero direct Unix/server sites does not mean zero transitive use.

| DLL | ntdll | kernel32 | user32 | gdi32 | Module closure | Direct server / Unix sites | PE evidence |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| advapi32 | 43 | 53 | 1 | 0 | 58 | none observed / none observed | not-built |
| advpack | 7 | 50 | 0 | 0 | 58 | none observed / none observed | not-built |
| cabinet | 8 | 20 | 0 | 0 | 58 | none observed / none observed | not-built |
| comctl32 | 17 | 80 | 187 | 89 | 58 | none observed / none observed | not-built |
| comdlg32 | 18 | 61 | 118 | 32 | 58 | none observed / none observed | not-built |
| ddraw | 6 | 12 | 17 | 11 | 62 | none observed / none observed | not-built |
| dinput | 15 | 35 | 37 | 0 | 59 | none observed / none observed | not-built |
| dsound | 12 | 33 | 0 | 0 | 59 | none observed / none observed | not-built |
| imaadp32.acm | 0 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| lz32 | 0 | 0 | 0 | 0 | 59 | none observed / none observed | not-built |
| msacm32 | 7 | 27 | 13 | 0 | 58 | none observed / none observed | not-built |
| msadp32.acm | 1 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| msg711.acm | 0 | 5 | 1 | 0 | 59 | none observed / none observed | not-built |
| msrle32 | 1 | 8 | 2 | 0 | 59 | none observed / none observed | not-built |
| msvcrt | 189 | 168 | 2 | 0 | 58 | none observed / none observed | not-built |
| msvfw32 | 11 | 27 | 33 | 9 | 59 | none observed / none observed | not-built |
| msvidc32 | 2 | 4 | 1 | 0 | 59 | none observed / none observed | not-built |
| ole32 | 22 | 76 | 50 | 37 | 58 | none observed / none observed | not-built |
| oleaut32 | 33 | 66 | 9 | 33 | 58 | none observed / none observed | not-built |
| riched20 | 22 | 25 | 64 | 47 | 59 | none observed / none observed | not-built |
| riched32 | 0 | 1 | 3 | 0 | 60 | none observed / none observed | not-built |
| rpcrt4 | 23 | 51 | 1 | 0 | 58 | none observed / none observed | not-built |
| setupapi | 37 | 92 | 11 | 0 | 58 | none observed / none observed | not-built |
| shell32 | 39 | 148 | 134 | 17 | 58 | none observed / none observed | not-built |
| shlwapi | 21 | 83 | 55 | 7 | 58 | none observed / none observed | not-built |
| version | 4 | 16 | 0 | 0 | 58 | none observed / none observed | built-experiment |
| winmm | 19 | 61 | 24 | 0 | 58 | none observed / none observed | not-built |

| Core | Unique source-reference candidates |
| --- | ---: |
| ntdll | 242 |
| kernel32 | 374 |
| user32 | 301 |
| gdi32 | 135 |

### Modern provider candidates

Names may overlap the four-core table; do not add these counts to it.

| Provider | Unique source-reference candidates |
| --- | ---: |
| combase | 54 |
| coml2 | 19 |
| kernelbase | 439 |
| sechost | 55 |
| shcore | 17 |
| win32u | 0 |

### Actual Wine 10.0 version.dll PE imports

| Provider | Count | Exact linked names |
| --- | ---: | --- |
| KERNEL32.DLL | 7 | `IsBadStringPtrA`, `LZClose`, `LZCopy`, `LZOpenFileA`, `MoveFileA`, `OpenFile`, `_lclose` |
| KERNELBASE.DLL | 21 | `DeleteFileA`, `DisableThreadLibraryCalls`, `GetFileAttributesA`, `GetFileVersionInfoA`, `GetFileVersionInfoExA`, `GetFileVersionInfoExW`, `GetFileVersionInfoSizeA`, `GetFileVersionInfoSizeExA`, `GetFileVersionInfoSizeExW`, `GetFileVersionInfoSizeW`, `GetFileVersionInfoW`, `GetTempFileNameA`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `VerFindFileA`, `VerFindFileW`, `VerQueryValueA`, `VerQueryValueW`, `WideCharToMultiByte`, `lstrcmpiA` |
| NTDLL.DLL | 3 | `__wine_dbg_header`, `__wine_dbg_output`, `__wine_dbg_strdup` |
| UCRTBASE.DLL | 4 | `__stdio_common_vsprintf`, `strcpy`, `strlen`, `strrchr` |

| Export | PE forwarder |
| --- | --- |
| VerLanguageNameA | `kernel32.VerLanguageNameA` |
| VerLanguageNameW | `kernel32.VerLanguageNameW` |

SHA-256 `f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32`; file 40960 bytes; SizeOfImage 32768 bytes; resource directory 948 bytes.

### NTDLL Unix backend request survey

These are exact request tokens in the three pinned files below, with line
locations in JSON. This survey is not the reachable protocol of any root DLL.

| Source file | Count | Named requests |
| --- | ---: | --- |
| dlls/ntdll/unix/file.c | 35 | `add_fd_completion`, `cancel_async`, `cancel_sync`, `create_file`, `create_mailslot`, `create_named_pipe`, `flush`, `get_async_result`, `get_directory_cache_entry`, `get_file_info`, `get_handle_unix_name`, `get_object_info`, `get_object_name`, `get_object_type`, `get_object_types`, `get_volume_info`, `ioctl`, `lock_file`, `open_file_object`, `read`, `read_change`, `read_directory_changes`, `register_async`, `set_async_direct_result`, `set_completion_info`, `set_fd_completion_mode`, `set_fd_disp_info`, `set_fd_eof_info`, `set_fd_name_info`, `set_handle_info`, `set_mailslot_info`, `set_named_pipe_info`, `set_serial_info`, `unlock_file`, `write` |
| dlls/ntdll/unix/server.c | 12 | `alloc_file_handle`, `allocate_reserve_object`, `close_handle`, `compare_objects`, `dup_handle`, `get_apc_result`, `get_handle_fd`, `init_first_thread`, `init_process_done`, `init_thread`, `queue_apc`, `select` |
| dlls/ntdll/unix/sync.c | 49 | `add_atom`, `add_completion`, `assign_job`, `cancel_timer`, `create_completion`, `create_debug_obj`, `create_directory`, `create_event`, `create_job`, `create_keyed_event`, `create_mapping`, `create_mutex`, `create_semaphore`, `create_symlink`, `create_timer`, `delete_atom`, `event_op`, `find_atom`, `get_atom_information`, `get_directory_entries`, `get_job_info`, `get_thread_completion`, `get_timer_info`, `open_completion`, `open_directory`, `open_event`, `open_job`, `open_keyed_event`, `open_mapping`, `open_mutex`, `open_semaphore`, `open_symlink`, `open_timer`, `process_in_job`, `query_completion`, `query_event`, `query_mutex`, `query_semaphore`, `query_symlink`, `release_mutex`, `release_semaphore`, `remove_completion`, `set_debug_obj_info`, `set_job_completion_port`, `set_job_limits`, `set_object_permanence`, `set_timer`, `terminate_job`, `wait_debug_event` |

### Per-DLL export names

#### advapi32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 43 | `MD4Final`, `MD4Init`, `MD4Update`, `NtCreateFile`, `NtCurrentTeb`, `NtOpenFile`, `NtQueryObject`, `NtQuerySecurityObject`, `NtQuerySystemInformation`, `NtRenameKey`, `NtSetSecurityObject`, `RtlAddAccessAllowedAceEx`, `RtlAddAccessDeniedAceEx`, `RtlAddAce`, `RtlAddAuditAccessAceEx`, `RtlCreateAcl`, `RtlCreateSecurityDescriptor`, `RtlCreateUnicodeStringFromAsciiz`, `RtlDosPathNameToNtPathName_U`, `RtlFreeUnicodeString`, `RtlGetAce`, `RtlGetControlSecurityDescriptor`, `RtlInitUnicodeString`, `RtlInitUnicodeStringEx`, `RtlIsTextUnicode`, `RtlLengthSecurityDescriptor`, `RtlMakeSelfRelativeSD`, `RtlNtStatusToDosError`, `RtlQueryInformationAcl`, `RtlSelfRelativeToAbsoluteSD`, `RtlUniform`, `memcmp`, `memcpy`, `memset`, `strcpy`, `strlen`, `swprintf`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcsrchr` |
| kernel32 | 53 | `CloseHandle`, `CompareStringW`, `CreateFileW`, `CreateProcessAsUserW`, `DeviceIoControl`, `EnterCriticalSection`, `ExpandEnvironmentStringsW`, `FileTimeToSystemTime`, `FreeLibrary`, `GetComputerNameExW`, `GetComputerNameW`, `GetCurrentProcess`, `GetCurrentThread`, `GetEnvironmentVariableA`, `GetEnvironmentVariableW`, `GetFileAttributesW`, `GetLastError`, `GetProcAddress`, `GetSystemTimeAsFileTime`, `GetThreadLocale`, `GetVersion`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `LoadLibraryW`, `LocalAlloc`, `LocalFree`, `MultiByteToWideChar`, `OpenProcessToken`, `OpenThreadToken`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `RegCloseKey`, `RegCreateKeyExA`, `RegCreateKeyExW`, `RegDeleteKeyExA`, `RegDeleteKeyExW`, `RegDeleteValueW`, `RegEnumKeyExA`, `RegEnumKeyExW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSaveKeyExA`, `RegSaveKeyExW`, `RegSetValueExW`, `SetLastError`, `WideCharToMultiByte`, `lstrcatW`, `lstrcpyW`, `lstrlenW` |
| user32 | 1 | `GetUserObjectSecurity` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 30; `-import` exports: 196. Exact targets, potential providers and source locations are in the JSON census.

#### advpack

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 7 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `memmove`, `strlen`, `wcschr`, `wcsrchr`, `wcstol` |
| kernel32 | 50 | `CloseHandle`, `CopyFileW`, `CreateFileW`, `CreateProcessW`, `DeleteFileW`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceW`, `FreeLibrary`, `FreeResource`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesW`, `GetLastError`, `GetModuleFileNameW`, `GetProcAddress`, `GetTempFileNameW`, `GetTempPathW`, `GetWindowsDirectoryW`, `LoadLibraryA`, `LoadLibraryExW`, `LoadResource`, `LockResource`, `MultiByteToWideChar`, `OpenProcessToken`, `RegCloseKey`, `RegCreateKeyExW`, `RegOpenKeyExW`, `RegQueryValueExW`, `RegSetValueExW`, `RemoveDirectoryW`, `SetFileAttributesW`, `SizeofResource`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpW`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### cabinet

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 8 | `RtlUlongByteSwap`, `RtlUshortByteSwap`, `memcpy`, `memset`, `strcat`, `strcpy`, `strlen`, `strrchr` |
| kernel32 | 20 | `CloseHandle`, `CreateDirectoryA`, `CreateFileA`, `DosDateTimeToFileTime`, `GetFileAttributesA`, `GetLastError`, `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `LocalFileTimeToFileTime`, `ReadFile`, `SetFilePointer`, `SetFileTime`, `SetLastError`, `WriteFile`, `lstrcatA`, `lstrcmpiA`, `lstrcpyA`, `lstrcpynA`, `lstrlenA` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### comctl32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 17 | `RtlMoveMemory`, `abs`, `floor`, `isdigit`, `iswalnum`, `memcpy`, `memmove`, `memset`, `qsort`, `strcat`, `strcpy`, `strlen`, `wcschr`, `wcsncmp`, `wcsrchr`, `wcsstr`, `wcstol` |
| kernel32 | 80 | `CloseHandle`, `CompareFileTime`, `CompareStringA`, `CompareStringW`, `CreateEventW`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceA`, `FindResourceW`, `FreeResource`, `GetCurrentThreadId`, `GetDateFormatW`, `GetDriveTypeW`, `GetLastError`, `GetLocalTime`, `GetLocaleInfoW`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetProcAddress`, `GetProcessHeap`, `GetThreadLocale`, `GetTickCount`, `GetUserDefaultLCID`, `GetVersion`, `GlobalAddAtomW`, `GlobalAlloc`, `GlobalDeleteAtom`, `GlobalFree`, `GlobalLock`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadCodePtr`, `IsBadStringPtrA`, `IsBadStringPtrW`, `IsBadWritePtr`, `IsValidLocale`, `LeaveCriticalSection`, `LoadLibraryW`, `LoadResource`, `LocalAlloc`, `LocalFree`, `LocalLock`, `LocalReAlloc`, `LocalSize`, `LocalUnlock`, `LockResource`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegCreateKeyExW`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExW`, `RtlMoveMemory`, `SetEvent`, `SetLastError`, `SizeofResource`, `SystemTimeToFileTime`, `WaitForSingleObject`, `WideCharToMultiByte`, `lstrcatW`, `lstrcmpW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 187 | `AdjustWindowRect`, `AdjustWindowRectEx`, `AppendMenuW`, `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcA`, `CallWindowProcW`, `CharLowerBuffW`, `CharLowerW`, `CharUpperBuffW`, `CharUpperW`, `CheckMenuItem`, `ChildWindowFromPoint`, `ClientToScreen`, `CloseClipboard`, `CopyIcon`, `CopyImage`, `CopyRect`, `CreateCaret`, `CreateDialogIndirectParamA`, `CreateDialogIndirectParamW`, `CreateIconIndirect`, `CreatePopupMenu`, `CreateWindowExW`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyCaret`, `DestroyCursor`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DispatchMessageW`, `DragDetect`, `DrawEdge`, `DrawFocusRect`, `DrawFrameControl`, `DrawIcon`, `DrawIconEx`, `DrawStateW`, `DrawTextA`, `DrawTextW`, `EditWndProc`, `EmptyClipboard`, `EnableMenuItem`, `EnableScrollBar`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `EnumChildWindows`, `EqualRect`, `FillRect`, `FrameRect`, `GetActiveWindow`, `GetAsyncKeyState`, `GetCapture`, `GetCaretPos`, `GetClassLongW`, `GetClassNameW`, `GetClientRect`, `GetClipboardData`, `GetCursorPos`, `GetDC`, `GetDCEx`, `GetDesktopWindow`, `GetDlgCtrlID`, `GetDlgItem`, `GetDoubleClickTime`, `GetDpiForWindow`, `GetFocus`, `GetIconInfo`, `GetKeyNameTextW`, `GetKeyState`, `GetMenuState`, `GetMessagePos`, `GetMessageTime`, `GetMessageW`, `GetMonitorInfoW`, `GetNextDlgGroupItem`, `GetNextDlgTabItem`, `GetParent`, `GetPropW`, `GetScrollInfo`, `GetScrollPos`, `GetScrollRange`, `GetSubMenu`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMenu`, `GetSystemMetrics`, `GetTopWindow`, `GetWindow`, `GetWindowDC`, `GetWindowLongW`, `GetWindowRect`, `GetWindowRgn`, `GetWindowTextA`, `GetWindowTextLengthA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InflateRect`, `InternalGetWindowText`, `IntersectRect`, `InvalidateRect`, `InvalidateRgn`, `InvertRect`, `IsChild`, `IsClipboardFormatAvailable`, `IsDialogMessageW`, `IsRectEmpty`, `IsWindow`, `IsWindowEnabled`, `IsWindowUnicode`, `IsWindowVisible`, `KillSystemTimer`, `KillTimer`, `LoadBitmapW`, `LoadCursorW`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadMenuA`, `LoadStringW`, `MapDialogRect`, `MapVirtualKeyW`, `MapWindowPoints`, `MonitorFromRect`, `MonitorFromWindow`, `MoveWindow`, `NotifyWinEvent`, `OffsetRect`, `OpenClipboard`, `PeekMessageW`, `PostMessageW`, `PostQuitMessage`, `PtInRect`, `RedrawWindow`, `RegisterClassW`, `RegisterWindowMessageW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropW`, `ScreenToClient`, `ScrollWindow`, `ScrollWindowEx`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageW`, `SetCapture`, `SetCaretPos`, `SetClipboardData`, `SetCursor`, `SetFocus`, `SetParent`, `SetPropW`, `SetRect`, `SetRectEmpty`, `SetScrollInfo`, `SetScrollPos`, `SetScrollRange`, `SetSystemTimer`, `SetTimer`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowPos`, `SetWindowRgn`, `SetWindowTextA`, `SetWindowTextW`, `ShowCaret`, `ShowScrollBar`, `ShowWindow`, `SubtractRect`, `SystemParametersInfoW`, `TabbedTextOutW`, `TrackMouseEvent`, `TrackPopupMenu`, `TranslateMessage`, `UnionRect`, `UnregisterClassA`, `UnregisterClassW`, `UpdateWindow`, `WindowFromPoint`, `mouse_event`, `wsprintfW` |
| gdi32 | 89 | `BitBlt`, `CombineRgn`, `CreateBitmap`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateFontIndirectW`, `CreatePatternBrush`, `CreatePen`, `CreatePenIndirect`, `CreatePolygonRgn`, `CreateRectRgn`, `CreateRectRgnIndirect`, `CreateRoundRectRgn`, `CreateSolidBrush`, `DPtoLP`, `DeleteDC`, `DeleteObject`, `ExcludeClipRect`, `ExtCreatePen`, `ExtCreateRegion`, `ExtSelectClipRgn`, `ExtTextOutW`, `FillRgn`, `FrameRgn`, `GdiAlphaBlend`, `GdiGetCharDimensions`, `GdiGetCodePage`, `GetBkColor`, `GetBkMode`, `GetCharABCWidthsW`, `GetCharWidthInfo`, `GetCharWidthW`, `GetClipBox`, `GetClipRgn`, `GetCurrentObject`, `GetDIBColorTable`, `GetDIBits`, `GetDeviceCaps`, `GetLayout`, `GetNearestColor`, `GetObjectType`, `GetObjectW`, `GetPixel`, `GetStockObject`, `GetTextCharsetInfo`, `GetTextColor`, `GetTextExtentExPointW`, `GetTextExtentPoint32A`, `GetTextExtentPoint32W`, `GetTextExtentPointA`, `GetTextExtentPointW`, `GetTextMetricsW`, `IntersectClipRect`, `LineTo`, `MoveToEx`, `OffsetWindowOrgEx`, `PatBlt`, `PlayEnhMetaFile`, `PolyPolyline`, `Polygon`, `RectVisible`, `Rectangle`, `ScriptBreak`, `ScriptStringAnalyse`, `ScriptStringCPtoX`, `ScriptStringFree`, `ScriptStringGetLogicalWidths`, `ScriptStringOut`, `ScriptStringXtoCP`, `ScriptString_pSize`, `ScriptString_pcOutChars`, `SelectClipRgn`, `SelectObject`, `SelectPalette`, `SetBkColor`, `SetBkMode`, `SetDIBColorTable`, `SetDIBits`, `SetPixel`, `SetPolyFillMode`, `SetRectRgn`, `SetTextAlign`, `SetTextColor`, `SetViewportOrgEx`, `SetWindowOrgEx`, `StretchBlt`, `StretchDIBits`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 31; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### comdlg32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 18 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `atoi`, `iswdigit`, `memcpy`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strrchr`, `swprintf`, `toupper`, `wcschr`, `wcspbrk`, `wcsrchr`, `wcstol` |
| kernel32 | 61 | `ActivateActCtx`, `CreateActCtxW`, `DeactivateActCtx`, `DisableThreadLibraryCalls`, `ExpandEnvironmentStringsW`, `FindClose`, `FindFirstFileW`, `FindResourceA`, `FindResourceW`, `FormatMessageW`, `GetCurrentActCtx`, `GetCurrentDirectoryA`, `GetCurrentDirectoryW`, `GetDriveTypeA`, `GetFileAttributesW`, `GetFullPathNameW`, `GetLastError`, `GetLocaleInfoW`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetProcessHeap`, `GetVersionExW`, `GlobalAlloc`, `GlobalFlags`, `GlobalFree`, `GlobalLock`, `GlobalReAlloc`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InterlockedDecrement`, `InterlockedIncrement`, `LoadResource`, `LocalFree`, `LockResource`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegGetValueW`, `RegOpenKeyExW`, `RegQueryValueExW`, `RegSetValueExW`, `ReleaseActCtx`, `SetCurrentDirectoryW`, `SetLastError`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `WideCharToMultiByte`, `lstrcatW`, `lstrcmpA`, `lstrcmpW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 118 | `AdjustWindowRectEx`, `AppendMenuW`, `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcA`, `CallWindowProcW`, `CharLowerW`, `CheckDlgButton`, `CheckRadioButton`, `ClientToScreen`, `CreateDialogIndirectParamA`, `CreateDialogIndirectParamW`, `CreatePopupMenu`, `CreateWindowExA`, `CreateWindowExW`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamA`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DlgDirListComboBoxW`, `DlgDirListW`, `DrawEdge`, `DrawFocusRect`, `DrawIconEx`, `DrawTextW`, `EnableMenuItem`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `EqualRect`, `FillRect`, `GetClassInfoW`, `GetClassLongW`, `GetClientRect`, `GetDC`, `GetDialogBaseUnits`, `GetDlgCtrlID`, `GetDlgItem`, `GetDlgItemInt`, `GetDlgItemTextA`, `GetDlgItemTextW`, `GetFocus`, `GetMenuItemInfoW`, `GetParent`, `GetPropA`, `GetPropW`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMetrics`, `GetWindow`, `GetWindowInfo`, `GetWindowLongW`, `GetWindowRect`, `GetWindowTextA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InflateRect`, `InsertMenuItemW`, `InvalidateRect`, `IsChild`, `IsDlgButtonChecked`, `IsWindow`, `IsWindowVisible`, `LoadCursorA`, `LoadCursorW`, `LoadIconA`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadStringA`, `LoadStringW`, `MapWindowPoints`, `MessageBoxW`, `MoveWindow`, `PeekMessageW`, `PostMessageA`, `PtInRect`, `RedrawWindow`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterWindowMessageA`, `RegisterWindowMessageW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropA`, `RemovePropW`, `ScreenToClient`, `SendDlgItemMessageA`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageW`, `SetCapture`, `SetCursor`, `SetDlgItemInt`, `SetDlgItemTextA`, `SetDlgItemTextW`, `SetFocus`, `SetMenuInfo`, `SetParent`, `SetPropA`, `SetPropW`, `SetRect`, `SetRectEmpty`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `SystemParametersInfoW`, `TrackPopupMenu`, `UpdateWindow`, `wsprintfW` |
| gdi32 | 32 | `BitBlt`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDCA`, `CreateDCW`, `CreateFontIndirectW`, `CreateICA`, `CreateICW`, `CreatePen`, `CreateRectRgnIndirect`, `CreateSolidBrush`, `DeleteDC`, `DeleteObject`, `EnumFontFamiliesExW`, `ExtTextOutW`, `GdiConvertToDevmodeW`, `GetDeviceCaps`, `GetNearestColor`, `GetObjectW`, `GetStockObject`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `LineTo`, `MoveToEx`, `Polygon`, `Rectangle`, `SelectClipRgn`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetTextColor`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### ddraw

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 6 | `fabs`, `memcmp`, `memcpy`, `memmove`, `memset`, `strcpy` |
| kernel32 | 12 | `DisableThreadLibraryCalls`, `GetLastError`, `GetModuleHandleExW`, `GetTickCount`, `GetVersion`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadReadPtr`, `RegCloseKey`, `RegQueryValueExA`, `WideCharToMultiByte` |
| user32 | 17 | `ClipCursor`, `CreateWindowExA`, `DefWindowProcA`, `DestroyWindow`, `GetDC`, `GetDesktopWindow`, `GetForegroundWindow`, `GetSystemMetrics`, `IsRectEmpty`, `RedrawWindow`, `RegisterClassA`, `ReleaseDC`, `SetRect`, `SetRectEmpty`, `SetWindowPos`, `ShowWindow`, `UnregisterClassA` |
| gdi32 | 11 | `BitBlt`, `CombineRgn`, `CreateRectRgn`, `CreateRectRgnIndirect`, `DeleteObject`, `ExtCreateRegion`, `GetDCOrgEx`, `GetRandomRgn`, `GetRegionData`, `GetStockObject`, `OffsetRgn` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### dinput

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 15 | `atan2`, `cos`, `memcmp`, `memcpy`, `memset`, `sin`, `sprintf`, `sqrt`, `strcat`, `strrchr`, `swprintf`, `wcscmp`, `wcscpy`, `wcslen`, `wcsstr` |
| kernel32 | 35 | `CancelIoEx`, `CloseHandle`, `CreateEventW`, `CreateFileW`, `CreateProcessW`, `CreateThread`, `DeleteCriticalSection`, `DeviceIoControl`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FreeLibraryAndExitThread`, `GetCurrentProcessId`, `GetCurrentThread`, `GetLastError`, `GetModuleFileNameA`, `GetModuleHandleExA`, `GetOverlappedResult`, `GetTickCount64`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `MulDiv`, `MultiByteToWideChar`, `ReadFile`, `RegCloseKey`, `RegDeleteTreeW`, `RegQueryValueExW`, `RegSetValueExW`, `SetEvent`, `SetThreadDescription`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `lstrcpynW` |
| user32 | 37 | `CallNextHookEx`, `ClipCursor`, `DefWindowProcW`, `DestroyWindow`, `DialogBoxParamW`, `DispatchMessageW`, `EndDialog`, `GetClientRect`, `GetCursorPos`, `GetDesktopWindow`, `GetDlgItem`, `GetForegroundWindow`, `GetKeyNameTextW`, `GetKeyState`, `GetKeyboardType`, `GetRawInputData`, `GetSystemMetrics`, `GetWindowLongW`, `GetWindowThreadProcessId`, `IsWindow`, `LoadStringW`, `MapVirtualKeyW`, `MapWindowPoints`, `MsgWaitForMultipleObjectsEx`, `PeekMessageW`, `PostMessageW`, `RegisterClassExW`, `RegisterRawInputDevices`, `SendDlgItemMessageW`, `SendMessageW`, `SetCursorPos`, `SetWindowLongW`, `SetWindowsHookExW`, `ShowCursor`, `TranslateMessage`, `UnhookWindowsHookEx`, `UnregisterClassW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### dsound

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 12 | `RtlUlongByteSwap`, `RtlUshortByteSwap`, `atoi`, `cos`, `log`, `memcpy`, `memset`, `pow`, `qsort`, `sqrt`, `strcat`, `strrchr` |
| kernel32 | 33 | `AcquireSRWLockExclusive`, `AcquireSRWLockShared`, `CloseHandle`, `CreateEventW`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `GetCurrentThread`, `GetLastError`, `GetModuleFileNameA`, `GetModuleHandleExW`, `GetTickCount`, `InitializeCriticalSectionEx`, `InitializeSRWLock`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegQueryValueExA`, `ReleaseSRWLockExclusive`, `ReleaseSRWLockShared`, `SetEvent`, `SetThreadDescription`, `SetThreadPriority`, `WaitForSingleObject`, `WideCharToMultiByte`, `lstrcmpW`, `lstrcpynW`, `lstrlenW` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### imaadp32.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### lz32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 0 | — |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 12; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msacm32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 7 | `memcmp`, `memcpy`, `memset`, `strlen`, `swprintf`, `wcschr`, `wcsstr` |
| kernel32 | 27 | `DisableThreadLibraryCalls`, `GetPrivateProfileSectionW`, `GetProcessHeap`, `GetVersionExA`, `HeapAlloc`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadReadPtr`, `MultiByteToWideChar`, `RegCloseKey`, `RegEnumKeyExW`, `RegEnumValueW`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `SetLastError`, `WideCharToMultiByte`, `lstrcmpW`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 13 | `CharLowerW`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `GetPropW`, `MessageBoxA`, `RegisterWindowMessageW`, `SendDlgItemMessageW`, `SendMessageW`, `SetPropW`, `ShowWindow`, `wsprintfW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msadp32.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 1 | `memcpy` |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msg711.acm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 5 | `GetProcessHeap`, `HeapAlloc`, `HeapFree`, `MultiByteToWideChar`, `lstrcpyW` |
| user32 | 1 | `MessageBoxA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msrle32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 1 | `memcpy` |
| kernel32 | 8 | `DisableThreadLibraryCalls`, `GlobalAlloc`, `GlobalFree`, `GlobalHandle`, `GlobalLock`, `GlobalUnlock`, `LocalAlloc`, `LocalFree` |
| user32 | 2 | `LoadStringW`, `MessageBoxW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvcrt

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 189 | `LdrAddRefDll`, `NtCurrentTeb`, `NtQueryInformationFile`, `NtQuerySystemTime`, `RtlAreBitsClear`, `RtlAreBitsSet`, `RtlCaptureContext`, `RtlInitializeBitMap`, `RtlPcToFileHeader`, `RtlSetBits`, `RtlTimeToSecondsSince1970`, `RtlUnwind`, `RtlWaitOnAddress`, `RtlWakeAddressSingle`, `_CIcos`, `_CIlog`, `_CIpow`, `_CIsin`, `_CIsqrt`, `__isascii`, `__iscsym`, `__iscsymf`, `__toascii`, `_atoi64`, `_errno`, `_ftol`, `_i64toa`, `_i64toa_s`, `_i64tow`, `_i64tow_s`, `_itoa`, `_itoa_s`, `_itow`, `_itow_s`, `_lfind`, `_ltoa`, `_ltoa_s`, `_ltow`, `_ltow_s`, `_makepath_s`, `_memccpy`, `_memicmp`, `_snprintf`, `_snprintf_s`, `_snwprintf`, `_snwprintf_s`, `_splitpath`, `_splitpath_s`, `_stricmp`, `_strlwr`, `_strlwr_s`, `_strnicmp`, `_strupr`, `_strupr_s`, `_swprintf`, `_tolower`, `_toupper`, `_ui64toa`, `_ui64toa_s`, `_ui64tow`, `_ui64tow_s`, `_ultoa`, `_ultoa_s`, `_ultow`, `_ultow_s`, `_vscprintf`, `_vscwprintf`, `_vsnprintf`, `_vsnprintf_s`, `_vsnwprintf`, `_vsnwprintf_s`, `_vswprintf`, `_wcsicmp`, `_wcslwr`, `_wcslwr_s`, `_wcsnicmp`, `_wcstoi64`, `_wcstoui64`, `_wcsupr`, `_wcsupr_s`, `_wmakepath_s`, `_wsplitpath_s`, `_wtoi`, `_wtoi64`, `_wtol`, `abs`, `atan`, `atan2`, `atoi`, `atol`, `bsearch`, `bsearch_s`, `ceil`, `cos`, `fabs`, `floor`, `isalnum`, `isalpha`, `iscntrl`, `isdigit`, `isgraph`, `islower`, `isprint`, `ispunct`, `isspace`, `isupper`, `iswalnum`, `iswalpha`, `iswascii`, `iswctype`, `iswdigit`, `iswgraph`, `iswlower`, `iswprint`, `iswspace`, `iswxdigit`, `isxdigit`, `labs`, `log`, `mbstowcs`, `memchr`, `memcmp`, `memcpy`, `memcpy_s`, `memmove`, `memmove_s`, `memset`, `pow`, `qsort`, `qsort_s`, `sin`, `sprintf`, `sprintf_s`, `sqrt`, `sscanf`, `strcat`, `strcat_s`, `strchr`, `strcmp`, `strcpy`, `strcpy_s`, `strcspn`, `strlen`, `strncat`, `strncat_s`, `strncmp`, `strncpy`, `strncpy_s`, `strnlen`, `strpbrk`, `strrchr`, `strspn`, `strstr`, `strtok_s`, `strtol`, `strtoul`, `swprintf`, `swprintf_s`, `tan`, `tolower`, `toupper`, `towlower`, `towupper`, `vsprintf`, `vsprintf_s`, `vswprintf_s`, `wcscat`, `wcscat_s`, `wcschr`, `wcscmp`, `wcscpy`, `wcscpy_s`, `wcscspn`, `wcslen`, `wcsncat`, `wcsncat_s`, `wcsncmp`, `wcsncpy`, `wcsncpy_s`, `wcsnlen`, `wcspbrk`, `wcsrchr`, `wcsspn`, `wcsstr`, `wcstok`, `wcstok_s`, `wcstol`, `wcstombs`, `wcstoul` |
| kernel32 | 168 | `Beep`, `CloseHandle`, `CloseThreadpoolTimer`, `CloseThreadpoolWork`, `CompareStringA`, `CompareStringW`, `CreateDirectoryW`, `CreateFileA`, `CreateFileW`, `CreatePipe`, `CreateProcessW`, `CreateThread`, `CreateThreadpoolTimer`, `CreateThreadpoolWork`, `DebugBreak`, `DecodePointer`, `DeleteCriticalSection`, `DeleteFileW`, `DuplicateHandle`, `EncodePointer`, `EnterCriticalSection`, `EnumSystemLocalesEx`, `ExitProcess`, `ExitThread`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileA`, `FindFirstFileW`, `FindNextFileA`, `FindNextFileW`, `FlsAlloc`, `FlsFree`, `FlsGetValue`, `FlsSetValue`, `FlushFileBuffers`, `FreeEnvironmentStringsA`, `FreeEnvironmentStringsW`, `FreeLibrary`, `FreeLibraryAndExitThread`, `GetACP`, `GetCPInfo`, `GetCommandLineA`, `GetCommandLineW`, `GetConsoleMode`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`, `GetDateFormatW`, `GetDiskFreeSpaceW`, `GetDriveTypeW`, `GetEnvironmentStringsA`, `GetEnvironmentStringsW`, `GetEnvironmentVariableW`, `GetExitCodeProcess`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSizeEx`, `GetFileType`, `GetFullPathNameW`, `GetLastError`, `GetLocalTime`, `GetLocaleInfoA`, `GetLocaleInfoEx`, `GetLocaleInfoW`, `GetModuleFileNameA`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetModuleHandleExW`, `GetModuleHandleW`, `GetNumberOfConsoleInputEvents`, `GetOEMCP`, `GetProcAddress`, `GetProcessHeap`, `GetStartupInfoA`, `GetStartupInfoW`, `GetStdHandle`, `GetStringTypeExA`, `GetStringTypeW`, `GetSystemInfo`, `GetSystemTimeAsFileTime`, `GetSystemTimePreciseAsFileTime`, `GetTempFileNameW`, `GetTimeFormatW`, `GetTimeZoneInformation`, `GetUserDefaultLocaleName`, `GetVersion`, `GetVersionExW`, `HeapAlloc`, `HeapCompact`, `HeapCreate`, `HeapDestroy`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `HeapValidate`, `HeapWalk`, `InitOnceExecuteOnce`, `InitializeCriticalSection`, `InitializeCriticalSectionEx`, `InterlockedCompareExchange`, `InterlockedDecrement`, `InterlockedExchange`, `InterlockedFlushSList`, `InterlockedIncrement`, `InterlockedPushEntrySList`, `IsBadReadPtr`, `IsProcessorFeaturePresent`, `IsValidCodePage`, `IsValidLocaleName`, `LCIDToLocaleName`, `LCMapStringA`, `LCMapStringW`, `LeaveCriticalSection`, `LoadLibraryW`, `LocaleNameToLCID`, `LockFile`, `MoveFileExW`, `MultiByteToWideChar`, `PeekConsoleInputA`, `RaiseException`, `ReadConsoleA`, `ReadConsoleInputA`, `ReadConsoleInputW`, `ReadFile`, `RemoveDirectoryW`, `ResumeThread`, `RtlCaptureContext`, `RtlPcToFileHeader`, `RtlUnwind`, `SetConsoleCtrlHandler`, `SetConsoleMode`, `SetCurrentDirectoryW`, `SetEndOfFile`, `SetEnvironmentVariableW`, `SetErrorMode`, `SetEvent`, `SetFileAttributesW`, `SetFilePointer`, `SetFileTime`, `SetLastError`, `SetStdHandle`, `SetThreadpoolTimer`, `SetUnhandledExceptionFilter`, `Sleep`, `SubmitThreadpoolWork`, `SystemTimeToFileTime`, `SystemTimeToTzSpecificLocalTime`, `TerminateProcess`, `TlsAlloc`, `TlsFree`, `TlsGetValue`, `TlsSetValue`, `TryEnterCriticalSection`, `UnhandledExceptionFilter`, `UnlockFile`, `VerifyConsoleIoHandle`, `VirtualProtect`, `WaitForSingleObject`, `WaitForThreadpoolTimerCallbacks`, `WideCharToMultiByte`, `WriteConsoleA`, `WriteConsoleW`, `WriteFile`, `lstrcatW`, `lstrcpyW`, `lstrcpynA` |
| user32 | 2 | `GetActiveWindow`, `MessageBoxIndirectW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 3; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvfw32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 11 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `_strnicmp`, `abs`, `memcmp`, `memcpy`, `memset`, `strlen`, `tolower`, `wcschr`, `wcslen` |
| kernel32 | 27 | `DisableThreadLibraryCalls`, `FreeLibrary`, `GetLastError`, `GetModuleHandleW`, `GetPrivateProfileSectionA`, `GetPrivateProfileStringW`, `GetProcAddress`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalUnlock`, `LoadLibraryA`, `MulDiv`, `MultiByteToWideChar`, `RegCloseKey`, `RegDeleteValueA`, `RegEnumValueA`, `RegOpenKeyExA`, `RegSetValueExA`, `WideCharToMultiByte`, `lstrcatA`, `lstrcatW`, `lstrcmpW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 33 | `AdjustWindowRect`, `BeginPaint`, `CreateWindowExW`, `DefMDIChildProcW`, `DefWindowProcW`, `DestroyWindow`, `DialogBoxParamW`, `EnableWindow`, `EndDialog`, `EndPaint`, `GetClientRect`, `GetDlgItem`, `GetWindow`, `GetWindowLongW`, `InvalidateRect`, `IsWindowUnicode`, `KillTimer`, `LoadCursorW`, `LoadStringW`, `MessageBoxW`, `RegisterClassW`, `SendDlgItemMessageW`, `SendMessageW`, `SetCursor`, `SetFocus`, `SetTimer`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `wsprintfA`, `wsprintfW` |
| gdi32 | 9 | `CreateCompatibleDC`, `CreateDIBSection`, `CreateHalftonePalette`, `DeleteDC`, `DeleteObject`, `RealizePalette`, `SelectObject`, `SelectPalette`, `StretchBlt` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### msvidc32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 2 | `memcpy`, `memset` |
| kernel32 | 4 | `DisableThreadLibraryCalls`, `GetProcessHeap`, `HeapAlloc`, `HeapFree` |
| user32 | 1 | `LoadStringW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### ole32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 22 | `NtClose`, `NtCreateKey`, `NtCurrentTeb`, `NtOpenKey`, `RtlInitUnicodeString`, `RtlNtStatusToDosError`, `RtlUlongByteSwap`, `RtlUshortByteSwap`, `abs`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcpy`, `strlen`, `swprintf`, `towupper`, `wcschr`, `wcscmp`, `wcstol`, `wcstoul` |
| kernel32 | 76 | `ActivateActCtx`, `CloseHandle`, `CreateEventW`, `CreateFileMappingW`, `CreateFileW`, `DeactivateActCtx`, `DeleteCriticalSection`, `DeleteFileW`, `DuplicateHandle`, `EnterCriticalSection`, `ExpandEnvironmentStringsW`, `FindActCtxSectionGuid`, `FreeLibrary`, `GetACP`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSize`, `GetFullPathNameW`, `GetLastError`, `GetModuleHandleW`, `GetProcessHeap`, `GetTempFileNameW`, `GetTempPathW`, `GetThreadLocale`, `GetTickCount`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `InitOnceExecuteOnce`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `IsBadCodePtr`, `IsBadReadPtr`, `LeaveCriticalSection`, `LoadLibraryExW`, `LockFileEx`, `MapViewOfFile`, `MulDiv`, `MultiByteToWideChar`, `OpenProcess`, `RaiseException`, `ReadFile`, `RegCloseKey`, `RegEnumKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExW`, `RegSetValueExW`, `ResetEvent`, `SearchPathW`, `SetEndOfFile`, `SetEvent`, `SetFilePointerEx`, `Sleep`, `UnlockFileEx`, `UnmapViewOfFile`, `VirtualQuery`, `WideCharToMultiByte`, `WriteFile`, `lstrcatW`, `lstrcmpA`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 50 | `CallNextHookEx`, `CloseClipboard`, `CopyAcceleratorTableW`, `DefWindowProcW`, `DestroyWindow`, `DispatchMessageW`, `DrawIcon`, `EmptyClipboard`, `EnumClipboardFormats`, `GetClipboardData`, `GetClipboardFormatNameA`, `GetClipboardFormatNameW`, `GetClipboardSequenceNumber`, `GetDC`, `GetKeyState`, `GetKeyboardState`, `GetMenuItemCount`, `GetMessageW`, `GetParent`, `GetPropW`, `GetSubMenu`, `GetSystemMetrics`, `GetWindowLongA`, `GetWindowThreadProcessId`, `IsClipboardFormatAvailable`, `IsWindow`, `KillTimer`, `LoadCursorW`, `OpenClipboard`, `PostQuitMessage`, `RegisterClassExW`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `RemovePropW`, `SendMessageW`, `SetCapture`, `SetClipboardData`, `SetCursor`, `SetPropW`, `SetTimer`, `SetWindowLongW`, `SetWindowsHookExW`, `SystemParametersInfoW`, `UnhookWindowsHookEx`, `UnregisterClassW`, `WindowFromPoint`, `wsprintfW` |
| gdi32 | 37 | `BitBlt`, `CloseMetaFile`, `CopyEnhMetaFileW`, `CopyMetaFileW`, `CreateBitmapIndirect`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBitmap`, `CreateFontIndirectW`, `CreateMetaFileW`, `CreatePalette`, `DeleteDC`, `DeleteEnhMetaFile`, `DeleteMetaFile`, `DeleteObject`, `ExtEscape`, `GetBitmapBits`, `GetDIBits`, `GetDeviceCaps`, `GetEnhMetaFileBits`, `GetMetaFileBitsEx`, `GetObjectW`, `GetPaletteEntries`, `GetTextExtentPoint32W`, `GetWinMetaFileBits`, `PlayMetaFile`, `SelectObject`, `SetEnhMetaFileBits`, `SetMapMode`, `SetMetaFileBitsEx`, `SetViewportExtEx`, `SetViewportOrgEx`, `SetWinMetaFileBits`, `SetWindowExtEx`, `SetWindowOrgEx`, `StretchDIBits`, `TextOutW` |

Private source names: none observed directly. Explicit forwarders: 134; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### oleaut32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 33 | `_strnicmp`, `bsearch`, `ceil`, `fabs`, `floor`, `isalnum`, `iswalpha`, `iswspace`, `labs`, `memcmp`, `memcpy`, `memmove`, `memset`, `pow`, `sprintf`, `sscanf`, `strcmp`, `strcpy`, `strlen`, `strtol`, `swprintf`, `towlower`, `towupper`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcsrchr`, `wcsspn`, `wcstol`, `wcstoul` |
| kernel32 | 66 | `CloseHandle`, `CompareStringW`, `ConvertDefaultLocale`, `CreateFileMappingW`, `CreateFileW`, `DebugBreak`, `EnterCriticalSection`, `FindActCtxSectionGuid`, `FindResourceExW`, `FindResourceW`, `FreeLibrary`, `FreeResource`, `GetCurrencyFormatW`, `GetCurrentProcess`, `GetEnvironmentVariableW`, `GetFileSize`, `GetFinalPathNameByHandleW`, `GetLastError`, `GetLocaleInfoW`, `GetNumberFormatW`, `GetProcAddress`, `GetProcessHeap`, `GetSystemDefaultLCID`, `GetSystemDirectoryW`, `GetSystemTime`, `GetTimeFormatW`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `HeapAlloc`, `HeapFree`, `InterlockedDecrement`, `InterlockedIncrement`, `IsWow64Process`, `LZClose`, `LZOpenFileW`, `LZRead`, `LZSeek`, `LeaveCriticalSection`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MapViewOfFile`, `MulDiv`, `MultiByteToWideChar`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteKeyExW`, `RegEnumKeyExA`, `RegEnumKeyExW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegSetValueExW`, `SearchPathW`, `SizeofResource`, `UnmapViewOfFile`, `WideCharToMultiByte`, `WriteFile`, `lstrcatW`, `lstrcmpiW`, `lstrcpyW`, `lstrcpynW`, `lstrlenW` |
| user32 | 9 | `CreateIconFromResourceEx`, `DestroyIcon`, `GetClientRect`, `GetDC`, `GetIconInfo`, `GetSysColor`, `GetWindowLongW`, `ReleaseDC`, `SetWindowLongW` |
| gdi32 | 33 | `BitBlt`, `CreateBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateDIBitmap`, `CreateFontIndirectW`, `DeleteDC`, `DeleteEnhMetaFile`, `DeleteMetaFile`, `DeleteObject`, `EnumFontFamiliesExW`, `GdiGetCharDimensions`, `GetCurrentObject`, `GetDIBits`, `GetDeviceCaps`, `GetEnhMetaFileHeader`, `GetObjectW`, `GetPaletteEntries`, `GetTextFaceW`, `GetTextMetricsW`, `PlayEnhMetaFile`, `PlayMetaFile`, `SelectObject`, `SetBkColor`, `SetEnhMetaFileBits`, `SetMapMode`, `SetMetaFileBitsEx`, `SetTextColor`, `SetViewportExtEx`, `SetViewportOrgEx`, `SetWindowExtEx`, `SetWindowOrgEx`, `StretchBlt` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 3. Exact targets, potential providers and source locations are in the JSON census.

#### riched20

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 22 | `abs`, `bsearch`, `isalnum`, `isalpha`, `isdigit`, `isupper`, `iswalnum`, `iswspace`, `isxdigit`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strlen`, `strncmp`, `swprintf`, `tolower`, `towupper`, `wcscmp` |
| kernel32 | 25 | `DisableThreadLibraryCalls`, `GetACP`, `GetCPInfo`, `GetCPInfoExW`, `GetLastError`, `GetOEMCP`, `GetSystemDefaultLCID`, `GetUserDefaultLangID`, `GetVersion`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalReAlloc`, `GlobalSize`, `GlobalUnlock`, `InterlockedDecrement`, `InterlockedIncrement`, `MulDiv`, `MultiByteToWideChar`, `WideCharToMultiByte`, `lstrcpyA`, `lstrcpyW`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 64 | `BeginPaint`, `ClientToScreen`, `CreateCaret`, `DefWindowProcW`, `DestroyCaret`, `DestroyMenu`, `EnableScrollBar`, `EndPaint`, `EqualRect`, `FillRect`, `FrameRect`, `GetClientRect`, `GetCursorPos`, `GetDC`, `GetDlgItem`, `GetDoubleClickTime`, `GetKeyState`, `GetMessageTime`, `GetParent`, `GetSysColor`, `GetSysColorBrush`, `GetSystemMetrics`, `GetUpdateRect`, `GetWindowLongW`, `HideCaret`, `InflateRect`, `IntersectRect`, `InvalidateRect`, `IsWindowVisible`, `KillTimer`, `LoadCursorW`, `MessageBeep`, `MessageBoxA`, `OffsetRect`, `PostMessageW`, `PtInRect`, `RegisterClassA`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `ScreenToClient`, `ScrollWindowEx`, `SendMessageW`, `SetActiveWindow`, `SetCapture`, `SetCaretPos`, `SetCursor`, `SetFocus`, `SetRect`, `SetRectEmpty`, `SetScrollInfo`, `SetScrollPos`, `SetTimer`, `SetWindowLongW`, `ShowCaret`, `ShowScrollBar`, `SystemParametersInfoW`, `TrackPopupMenu`, `UnregisterClassA`, `UnregisterClassW`, `UpdateWindow`, `wsprintfW` |
| gdi32 | 47 | `CreateCompatibleDC`, `CreateDIBitmap`, `CreateFontIndirectW`, `CreatePen`, `CreateRectRgn`, `CreateRectRgnIndirect`, `CreateSolidBrush`, `DeleteDC`, `DeleteObject`, `ExtCreatePen`, `ExtSelectClipRgn`, `ExtTextOutW`, `GetClipRgn`, `GetDeviceCaps`, `GetEnhMetaFileBits`, `GetEnhMetaFileHeader`, `GetObjectW`, `GetStockObject`, `GetTextExtentExPointW`, `GetTextExtentPoint32W`, `GetTextExtentPointW`, `GetTextMetricsW`, `IntersectClipRect`, `LineTo`, `MoveToEx`, `PatBlt`, `PlayEnhMetaFile`, `RectVisible`, `ScriptCPtoX`, `ScriptFreeCache`, `ScriptItemize`, `ScriptLayout`, `ScriptPlace`, `ScriptShape`, `ScriptTextOut`, `ScriptXtoCP`, `SelectClipRgn`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetEnhMetaFileBits`, `SetTextAlign`, `SetTextColor`, `SetWinMetaFileBits`, `StretchBlt`, `TextOutW`, `TranslateCharsetInfo` |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### riched32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 0 | — |
| kernel32 | 1 | `DisableThreadLibraryCalls` |
| user32 | 3 | `LoadCursorA`, `RegisterClassA`, `UnregisterClassA` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### rpcrt4

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 23 | `NtCancelIoFileEx`, `NtCurrentTeb`, `NtFsControlFile`, `NtReadFile`, `NtWriteFile`, `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `memcmp`, `memcpy`, `memmove`, `memset`, `sin`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `swprintf`, `wcschr`, `wcscmp`, `wcsncmp`, `wcstol` |
| kernel32 | 51 | `CancelIoEx`, `CloseHandle`, `CreateEventW`, `CreateFileA`, `CreateMutexW`, `CreateNamedPipeA`, `CreateThread`, `DeleteCriticalSection`, `EnterCriticalSection`, `ExitProcess`, `FlushFileBuffers`, `FormatMessageW`, `GetComputerNameA`, `GetCurrentProcessId`, `GetCurrentThread`, `GetCurrentThreadId`, `GetLastError`, `GetModuleFileNameW`, `GetNamedPipeClientProcessId`, `GetSystemTimeAsFileTime`, `GetThreadId`, `GetTickCount`, `GetTickCount64`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedExchangeAdd`, `InterlockedIncrement`, `LeaveCriticalSection`, `MultiByteToWideChar`, `PostQueuedCompletionStatus`, `QueueUserAPC`, `QueueUserWorkItem`, `RaiseException`, `RegCloseKey`, `RegDeleteTreeW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseMutex`, `ResetEvent`, `SetEvent`, `SetNamedPipeHandleState`, `SetThreadDescription`, `Sleep`, `WaitForMultipleObjects`, `WaitForMultipleObjectsEx`, `WaitForSingleObject`, `WaitNamedPipeA`, `WideCharToMultiByte`, `lstrcatW`, `lstrcpyW`, `lstrlenW` |
| user32 | 1 | `PostMessageW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### setupapi

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 37 | `LdrFindResourceDirectory_U`, `NtAllocateVirtualMemory`, `NtClose`, `NtDeleteKey`, `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `RtlImageDirectoryEntryToData`, `RtlImageNtHeader`, `RtlImageRvaToVa`, `RtlIsTextUnicode`, `RtlUnicodeToMultiByteN`, `RtlUnicodeToMultiByteSize`, `_vsnwprintf`, `iswspace`, `memchr`, `memcmp`, `memcpy`, `memset`, `sprintf`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `strrchr`, `strtol`, `swprintf`, `towlower`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcslen`, `wcsncmp`, `wcspbrk`, `wcsrchr`, `wcstol`, `wcstoul` |
| kernel32 | 92 | `CloseHandle`, `CopyFileW`, `CreateDirectoryW`, `CreateFileA`, `CreateFileMappingW`, `CreateFileW`, `CreateProcessW`, `DeleteFileW`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `EnumResourceNamesW`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FindResourceW`, `FormatMessageW`, `FreeLibrary`, `GetCurrentProcess`, `GetDiskFreeSpaceW`, `GetDriveTypeW`, `GetEnvironmentVariableW`, `GetFileAttributesW`, `GetFileSize`, `GetFileSizeEx`, `GetFileTime`, `GetFullPathNameA`, `GetFullPathNameW`, `GetLastError`, `GetLogicalDriveStringsW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProcessHeap`, `GetSystemDirectoryW`, `GetSystemInfo`, `GetTempFileNameW`, `GetTempPathW`, `GetThreadLocale`, `GetVersion`, `GetVersionExW`, `GetWindowsDirectoryW`, `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `IsWow64Process`, `LZClose`, `LZCopy`, `LZOpenFileW`, `LZSeek`, `LeaveCriticalSection`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MapViewOfFile`, `MoveFileExW`, `MoveFileW`, `MultiByteToWideChar`, `OpenProcessToken`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteTreeW`, `RegDeleteValueW`, `RegEnumKeyExW`, `RegGetValueW`, `RegOpenKeyExW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `RemoveDirectoryW`, `SetEndOfFile`, `SetErrorMode`, `SetFilePointer`, `SetLastError`, `SizeofResource`, `TerminateProcess`, `UnmapViewOfFile`, `VirtualFree`, `WaitForSingleObject`, `WideCharToMultiByte`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 11 | `CharLowerW`, `DialogBoxParamW`, `EndDialog`, `GetDlgItem`, `GetWindowLongW`, `GetWindowTextW`, `LoadStringW`, `SetDlgItemTextW`, `SetWindowLongW`, `SetWindowTextW`, `ShowWindow` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

#### shell32

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 39 | `RtlDosPathNameToNtPathName_U`, `RtlFreeUnicodeString`, `RtlGUIDFromString`, `RtlInitUnicodeString`, `RtlRandom`, `atoi`, `isprint`, `iswspace`, `memcmp`, `memcpy`, `memmove`, `memset`, `pow`, `qsort`, `sprintf`, `sscanf`, `strcat`, `strchr`, `strcmp`, `strcpy`, `strlen`, `strncmp`, `strrchr`, `swprintf`, `towlower`, `towupper`, `wcscat`, `wcschr`, `wcscmp`, `wcscpy`, `wcscpy_s`, `wcslen`, `wcsncmp`, `wcspbrk`, `wcsrchr`, `wcsstr`, `wcstol`, `wine_get_build_id`, `wine_get_host_version` |
| kernel32 | 148 | `ActivateActCtx`, `CloseHandle`, `CompareFileTime`, `CopyFileW`, `CreateActCtxW`, `CreateDirectoryW`, `CreateFileMappingW`, `CreateFileW`, `CreateProcessAsUserW`, `CreateProcessW`, `DeactivateActCtx`, `DeleteCriticalSection`, `DeleteFileA`, `DeleteFileW`, `DeviceIoControl`, `DisableThreadLibraryCalls`, `DosDateTimeToFileTime`, `EnterCriticalSection`, `ExpandEnvironmentStringsA`, `ExpandEnvironmentStringsW`, `FileTimeToDosDateTime`, `FileTimeToLocalFileTime`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileA`, `FindFirstFileW`, `FindNextFileA`, `FindNextFileW`, `FindResourceW`, `FormatMessageA`, `FormatMessageW`, `FreeEnvironmentStringsW`, `FreeLibrary`, `GetBinaryTypeW`, `GetCurrentDirectoryA`, `GetCurrentDirectoryW`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetDateFormatA`, `GetDateFormatW`, `GetDiskFreeSpaceExW`, `GetDriveTypeA`, `GetDriveTypeW`, `GetEnvironmentStringsW`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesExW`, `GetFileAttributesW`, `GetFileSize`, `GetFullPathNameA`, `GetFullPathNameW`, `GetLastError`, `GetLocalTime`, `GetLogicalDrives`, `GetModuleFileNameW`, `GetModuleHandleA`, `GetModuleHandleW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProcessHeap`, `GetProfileStringW`, `GetShortPathNameA`, `GetShortPathNameW`, `GetSystemDirectoryA`, `GetSystemDirectoryW`, `GetSystemWow64DirectoryW`, `GetTickCount`, `GetTimeFormatA`, `GetTimeFormatW`, `GetVersion`, `GetVolumeInformationA`, `GetVolumeInformationW`, `GetWindowsDirectoryW`, `GlobalAlloc`, `GlobalFree`, `GlobalLock`, `GlobalSize`, `GlobalUnlock`, `HeapAlloc`, `HeapFree`, `InitOnceExecuteOnce`, `InterlockedDecrement`, `InterlockedIncrement`, `IsWow64Process`, `LeaveCriticalSection`, `LoadLibraryA`, `LoadLibraryExW`, `LoadLibraryW`, `LoadResource`, `LocalAlloc`, `LocalFileTimeToFileTime`, `LocalFree`, `LockResource`, `MapViewOfFile`, `MoveFileExW`, `MoveFileW`, `MulDiv`, `MultiByteToWideChar`, `OpenProcessToken`, `ProcessIdToSessionId`, `ReadFile`, `RegCloseKey`, `RegCreateKeyExA`, `RegCreateKeyExW`, `RegEnumKeyExW`, `RegEnumValueA`, `RegEnumValueW`, `RegGetValueW`, `RegLoadMUIStringA`, `RegLoadMUIStringW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseActCtx`, `RemoveDirectoryW`, `SearchPathW`, `SetCurrentDirectoryA`, `SetCurrentDirectoryW`, `SetErrorMode`, `SetFileAttributesW`, `SetFilePointer`, `SetLastError`, `SizeofResource`, `Sleep`, `SystemTimeToFileTime`, `UnmapViewOfFile`, `WideCharToMultiByte`, `Wow64DisableWow64FsRedirection`, `Wow64RevertWow64FsRedirection`, `WriteFile`, `WritePrivateProfileStringW`, `lstrcatA`, `lstrcatW`, `lstrcmpA`, `lstrcmpiA`, `lstrcmpiW`, `lstrcpyA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW`, `wine_get_dos_file_name`, `wine_get_unix_file_name` |
| user32 | 134 | `BeginDeferWindowPos`, `BeginPaint`, `CallWindowProcW`, `CharLowerW`, `CheckMenuRadioItem`, `CreateIconIndirect`, `CreateMenu`, `CreatePopupMenu`, `CreateWindowExW`, `DdeClientTransaction`, `DdeConnect`, `DdeCreateDataHandle`, `DdeCreateStringHandleW`, `DdeDisconnect`, `DdeFreeDataHandle`, `DdeFreeStringHandle`, `DdeGetData`, `DdeGetLastError`, `DdeInitializeA`, `DdeInitializeW`, `DdeNameService`, `DdeQueryStringW`, `DdeUninitialize`, `DefWindowProcW`, `DeferWindowPos`, `DeleteMenu`, `DestroyIcon`, `DestroyMenu`, `DestroyWindow`, `DialogBoxIndirectParamW`, `DialogBoxParamW`, `DispatchMessageW`, `DrawEdge`, `DrawMenuBar`, `DrawTextW`, `EnableMenuItem`, `EnableWindow`, `EndDeferWindowPos`, `EndDialog`, `EndPaint`, `ExitWindowsEx`, `FindWindowExW`, `FindWindowW`, `GetActiveWindow`, `GetAncestor`, `GetCapture`, `GetClassInfoW`, `GetClientRect`, `GetDC`, `GetDCEx`, `GetDlgItem`, `GetDlgItemTextW`, `GetIconInfo`, `GetKeyState`, `GetMenu`, `GetMenuDefaultItem`, `GetMenuInfo`, `GetMenuItemCount`, `GetMenuItemID`, `GetMenuItemInfoW`, `GetMenuItemRect`, `GetMessageW`, `GetParent`, `GetPropW`, `GetSubMenu`, `GetSysColor`, `GetSystemMetrics`, `GetWindowLongA`, `GetWindowLongW`, `GetWindowRect`, `GetWindowTextA`, `GetWindowTextLengthW`, `GetWindowTextW`, `InsertMenuA`, `InsertMenuItemW`, `InsertMenuW`, `InvertRect`, `IsProcessDPIAware`, `IsWindow`, `IsWindowVisible`, `LoadAcceleratorsW`, `LoadCursorA`, `LoadCursorW`, `LoadIconW`, `LoadImageA`, `LoadImageW`, `LoadMenuA`, `LoadMenuW`, `LoadStringA`, `LoadStringW`, `MapWindowPoints`, `MessageBoxA`, `MessageBoxIndirectW`, `MessageBoxW`, `MoveWindow`, `PostQuitMessage`, `PrivateExtractIconExW`, `PrivateExtractIconsW`, `RegisterClassExW`, `RegisterClassW`, `RegisterClipboardFormatA`, `RegisterClipboardFormatW`, `ReleaseCapture`, `ReleaseDC`, `RemoveMenu`, `RemovePropW`, `ScreenToClient`, `SendDlgItemMessageW`, `SendMessageA`, `SendMessageTimeoutW`, `SendMessageW`, `SetCapture`, `SetCursor`, `SetDlgItemTextW`, `SetFocus`, `SetForegroundWindow`, `SetMenu`, `SetMenuDefaultItem`, `SetMenuInfo`, `SetPropW`, `SetRect`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowPos`, `SetWindowTextA`, `SetWindowTextW`, `ShowWindow`, `SystemParametersInfoW`, `TrackPopupMenu`, `TrackPopupMenuEx`, `TranslateMessage`, `UpdateWindow`, `WaitForInputIdle`, `wsprintfA` |
| gdi32 | 17 | `BitBlt`, `CreateCompatibleBitmap`, `CreateCompatibleDC`, `CreateDIBSection`, `CreateFontIndirectW`, `DeleteObject`, `ExtTextOutW`, `GetBitmapBits`, `GetDeviceCaps`, `GetObjectW`, `GetStockObject`, `GetTextExtentPoint32W`, `GetTextMetricsW`, `SelectObject`, `SetBkColor`, `SetBkMode`, `SetTextColor` |

Private source names: kernel32!wine_get_dos_file_name, kernel32!wine_get_unix_file_name, ntdll!wine_get_build_id, ntdll!wine_get_host_version. Explicit forwarders: 34; `-import` exports: 4. Exact targets, potential providers and source locations are in the JSON census.

#### shlwapi

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 21 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `floor`, `islower`, `iswlower`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strlen`, `strrchr`, `swprintf`, `tolower`, `towlower`, `wcscmp`, `wcslen`, `wcsrchr` |
| kernel32 | 83 | `CloseHandle`, `CreateFileMappingA`, `CreateSemaphoreW`, `CreateTimerQueueTimer`, `DisableThreadLibraryCalls`, `DuplicateHandle`, `FileTimeToSystemTime`, `FindClose`, `FindFirstFileW`, `FindNextFileW`, `FormatMessageW`, `FreeLibrary`, `GetCurrentProcess`, `GetCurrentProcessId`, `GetDateFormatW`, `GetEnvironmentVariableW`, `GetFileAttributesA`, `GetFileAttributesW`, `GetLastError`, `GetLocaleInfoW`, `GetModuleFileNameA`, `GetModuleFileNameW`, `GetModuleHandleW`, `GetNumberFormatW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetStringTypeW`, `GetSystemDefaultUILanguage`, `GetSystemDirectoryW`, `GetSystemWindowsDirectoryW`, `GetTickCount`, `GetTimeFormatW`, `GetUserDefaultUILanguage`, `GetVersion`, `GetWindowsDirectoryW`, `InterlockedDecrement`, `InterlockedIncrement`, `IsDBCSLeadByte`, `LoadLibraryA`, `LoadLibraryExA`, `LoadLibraryW`, `LocalAlloc`, `LocalFree`, `LocalReAlloc`, `LocalSize`, `MapViewOfFile`, `MultiByteToWideChar`, `OpenProcess`, `OpenProcessToken`, `OpenSemaphoreW`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `QueueUserWorkItem`, `RegCloseKey`, `RegCreateKeyExW`, `RegDeleteValueA`, `RegDeleteValueW`, `RegEnumValueA`, `RegEnumValueW`, `RegOpenKeyExA`, `RegOpenKeyExW`, `RegQueryInfoKeyA`, `RegQueryInfoKeyW`, `RegQueryValueExA`, `RegQueryValueExW`, `RegSetValueExA`, `RegSetValueExW`, `ReleaseSemaphore`, `SetErrorMode`, `SetFileAttributesA`, `SetFileAttributesW`, `SystemTimeToTzSpecificLocalTime`, `UnmapViewOfFile`, `WaitForSingleObject`, `WideCharToMultiByte`, `WritePrivateProfileStringW`, `lstrcatW`, `lstrcmpiA`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenA`, `lstrlenW` |
| user32 | 55 | `CharNextA`, `CharPrevA`, `CheckMenuItem`, `CreatePopupMenu`, `CreateWindowExA`, `CreateWindowExW`, `DefWindowProcA`, `DefWindowProcW`, `DestroyMenu`, `DialogBoxParamW`, `EnableMenuItem`, `EndDialog`, `EnumChildWindows`, `GetClassInfoA`, `GetClassInfoW`, `GetClientRect`, `GetDC`, `GetDlgItem`, `GetMenuDefaultItem`, `GetMenuItemCount`, `GetMenuItemInfoW`, `GetParent`, `GetPropA`, `GetSubMenu`, `GetSystemMetrics`, `GetWindowLongW`, `IsChild`, `IsWindowUnicode`, `LoadCursorA`, `LoadCursorW`, `LoadMenuW`, `LoadStringW`, `MessageBoxW`, `MsgWaitForMultipleObjectsEx`, `PeekMessageW`, `PostMessageA`, `PostMessageW`, `RegisterClassA`, `RegisterClassW`, `RegisterClipboardFormatA`, `ReleaseDC`, `RemoveMenu`, `RemovePropA`, `SendMessageA`, `SendMessageTimeoutA`, `SendMessageTimeoutW`, `SendMessageW`, `SetParent`, `SetWindowLongA`, `SetWindowLongW`, `SetWindowTextW`, `ShowWindow`, `UnregisterClassA`, `UnregisterClassW`, `wsprintfW` |
| gdi32 | 7 | `CreateHalftonePalette`, `DeleteObject`, `ExtTextOutA`, `GetDeviceCaps`, `GetTextExtentPointW`, `SelectObject`, `SetBkColor` |

Private source names: none observed directly. Explicit forwarders: 215; `-import` exports: 198. Exact targets, potential providers and source locations are in the JSON census.

#### version

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 4 | `sprintf`, `strcpy`, `strlen`, `strrchr` |
| kernel32 | 16 | `DeleteFileA`, `GetFileAttributesA`, `GetLastError`, `GetProcessHeap`, `GetTempFileNameA`, `HeapAlloc`, `HeapFree`, `LZClose`, `LZCopy`, `LZOpenFileA`, `MoveFileA`, `MultiByteToWideChar`, `OpenFile`, `WideCharToMultiByte`, `_lclose`, `lstrcmpiA` |
| user32 | 0 | — |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 2; `-import` exports: 12. Exact targets, potential providers and source locations are in the JSON census.

#### winmm

| Core | Count | Names |
| --- | ---: | --- |
| ntdll | 19 | `RtlCreateUnicodeStringFromAsciiz`, `RtlFreeUnicodeString`, `RtlUpperChar`, `memcmp`, `memcpy`, `memmove`, `memset`, `sprintf`, `strcat`, `strcmp`, `strcpy`, `strrchr`, `swprintf`, `wcschr`, `wcscmp`, `wcscpy`, `wcsrchr`, `wcsstr`, `wcstoul` |
| kernel32 | 61 | `CloseHandle`, `CreateEventW`, `CreateFileA`, `CreateThread`, `DeleteCriticalSection`, `DisableThreadLibraryCalls`, `EnterCriticalSection`, `FindResourceW`, `FreeLibrary`, `FreeLibraryAndExitThread`, `FreeResource`, `GetCurrentThreadId`, `GetDriveTypeW`, `GetFileAttributesA`, `GetFullPathNameA`, `GetLastError`, `GetModuleFileNameW`, `GetModuleHandleExW`, `GetPrivateProfileStringW`, `GetProcAddress`, `GetProfileStringW`, `GetTickCount`, `InitOnceExecuteOnce`, `InitializeCriticalSectionEx`, `InterlockedDecrement`, `InterlockedIncrement`, `LeaveCriticalSection`, `LoadLibraryW`, `LoadResource`, `LockResource`, `MoveFileA`, `MulDiv`, `MultiByteToWideChar`, `OpenFile`, `PulseEvent`, `QueryPerformanceCounter`, `QueryPerformanceFrequency`, `RegCloseKey`, `RegEnumKeyExW`, `RegOpenKeyExW`, `RegQueryInfoKeyW`, `RegQueryValueExW`, `ResetEvent`, `SearchPathA`, `SearchPathW`, `SetEvent`, `SetThreadPriority`, `Sleep`, `SleepConditionVariableCS`, `WaitForMultipleObjects`, `WaitForSingleObject`, `WakeConditionVariable`, `WideCharToMultiByte`, `_hwrite`, `_lclose`, `_llseek`, `_lread`, `lstrcpyW`, `lstrcpynA`, `lstrcpynW`, `lstrlenW` |
| user32 | 24 | `CharLowerW`, `CharUpperW`, `DefWindowProcW`, `DestroyWindow`, `DispatchMessageA`, `GetActiveWindow`, `GetAsyncKeyState`, `GetMessageA`, `IsWindow`, `KillTimer`, `LoadStringA`, `LoadStringW`, `MessageBoxA`, `MsgWaitForMultipleObjects`, `PeekMessageA`, `PeekMessageW`, `PostMessageA`, `PostMessageW`, `PostThreadMessageA`, `PostThreadMessageW`, `SendMessageA`, `SendMessageW`, `SetTimer`, `SetWindowLongW` |
| gdi32 | 0 | — |

Private source names: none observed directly. Explicit forwarders: 0; `-import` exports: 0. Exact targets, potential providers and source locations are in the JSON census.

### Recursive Makefile closure

Direct, delay and implicit default libraries are kept separately. Static
GUID/helper libraries and unresolved configure variables are not PE DLLs.
This is the build graph, including cycles; it is not a runtime working set.

| Module | Direct | Delay | Defaults | Unix companion | Configure variables |
| --- | --- | --- | --- | --- | --- |
| acledit | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| aclui | comctl32 user32 advapi32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| activeds | advapi32 ole32 oleaut32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| activeds.tlb | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| actxprxy | uuid oleaut32 ole32 rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| adsldp | ole32 oleaut32 secur32 activeds uuid | netapi32 wldap32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| adsldpc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| advapi32 | kernelbase sechost msvcrt | rpcrt4 user32 | winecrt0 msvcrt kernel32 ntdll | — | — |
| advpack | ole32 setupapi version advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| amsi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| amstream | strmiids strmbase uuid ole32 advapi32 ddraw msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| apisetschema | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| apphelp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| appwiz.cpl | uuid urlmon advpack comctl32 advapi32 shell32 ole32 user32 comdlg32 bcrypt kernelbase | msi | winecrt0 ucrtbase kernel32 ntdll | — | — |
| appxdeploymentclient | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atl | uuid oleaut32 ole32 user32 gdi32 advapi32 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atl100 | uuid ole32 oleaut32 user32 gdi32 advapi32 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atl110 | oleaut32 ole32 user32 gdi32 advapi32 uuid shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atl80 | oleaut32 user32 ole32 gdi32 advapi32 uuid shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atl90 | oleaut32 user32 ole32 gdi32 advapi32 uuid shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atlthunk | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| atmlib | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| authz | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| avicap32 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | avicap32.so | — |
| avifil32 | uuid msacm32 msvfw32 winmm ole32 user32 advapi32 rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| avrt | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| bcp47langs | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| bcrypt | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | bcrypt.so | — |
| bcryptprimitives | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| bluetoothapis | setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| browseui | uuid ole32 comctl32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| bthprops.cpl | bluetoothapis | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cabinet | — | — | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| capi2032 | — | — | winecrt0 ucrtbase kernel32 ntdll | capi2032.so | — |
| cards | user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cdosys | uuid ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cfgmgr32 | setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| clusapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cng.sys | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| colorcnv | combase mfplat msdmo mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| combase | advapi32 ole32 user32 gdi32 uuid rpcrt4 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comcat | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comctl32 | uuid user32 gdi32 advapi32 imm32 kernelbase | winmm uxtheme | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comdlg32 | uuid shell32 shlwapi comctl32 winspool user32 gdi32 advapi32 | ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| coml2 | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| compstui | comctl32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| comsvcs | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| concrt140 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| connect | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| coremessaging | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| credui | comctl32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| crtdll | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| crypt32 | user32 advapi32 bcrypt | cryptnet | winecrt0 ucrtbase kernel32 ntdll | crypt32.so | — |
| cryptdlg | cryptui crypt32 wintrust user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptdll | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptext | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptnet | crypt32 shell32 ole32 advapi32 | wininet | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptowinrt | combase bcrypt uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptsp | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| cryptui | uuid crypt32 ole32 comctl32 comdlg32 user32 gdi32 advapi32 | wintrust urlmon | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ctapi32 | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | ctapi32.so | — |
| ctl3d32 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d2d1 | d3d10_1 dxguid uuid gdi32 user32 advapi32 d3dcompiler | dwrite xmllite ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d10 | uuid d3d10core d3dcompiler_39 dxgi wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d10_1 | dxguid d3d10core dxgi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d10core | d3d11 dxgi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d11 | dxguid uuid dxgi wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d12 | dxgi dxguid wined3d gdi32 user32 uuid win32u | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d12core | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d8 | dxguid uuid wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d8thk | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3d9 | dxguid uuid wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_33 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_34 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_35 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_36 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_37 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_38 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_39 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_40 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_41 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_42 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_43 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_46 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dcompiler_47 | wined3d | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dim | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dim700 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3drm | d3dxof ddraw | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_33 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_34 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_35 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_36 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_37 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_38 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_39 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_40 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_41 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_42 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx10_43 | d3d10_1 d3dcompiler dxguid uuid gdi32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx11_42 | d3dcompiler | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx11_43 | d3dcompiler | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_24 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_25 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_26 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_27 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_28 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_29 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_30 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_31 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_32 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_33 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_34 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_35 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_36 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_37 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_38 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_39 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_40 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_41 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_42 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dx9_43 | d3d9 d3dcompiler dxguid d3dxof ole32 gdi32 user32 | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| d3dxof | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dataexchange | combase user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| davclnt | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dbgeng | — | version | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dbghelp | kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| dciman32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dcomp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ddraw | dxguid uuid wined3d user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ddrawex | dxguid uuid ddraw ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| desk.cpl | advapi32 ole32 comctl32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| devenum | dmoguids strmiids uuid ole32 oleaut32 avicap32 winmm user32 advapi32 dsound msdmo | msvfw32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dhcpcsvc | — | iphlpapi | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dhcpcsvc6 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dhtmled.ocx | uuid ole32 oleaut32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| diasymreader | uuid rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| difxapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dinput | dinput dxguid uuid comctl32 ole32 user32 advapi32 hid setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dinput8 | dinput8 dxguid uuid comctl32 ole32 user32 advapi32 hid setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| directmanipulation | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dispex | uuid oleaut32 ole32 rpcrt4 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmband | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmcompos | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmime | dxguid uuid dsound ole32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmloader | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmscript | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmstyle | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmsynth | dxguid uuid ole32 advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FLUIDSYNTH_PE_LIBS |
| dmusic | dxguid uuid ole32 advapi32 dsound user32 winmm | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dmusic32 | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dnsapi | — | netapi32 | winecrt0 ucrtbase kernel32 ntdll | dnsapi.so | — |
| dplay | dplayx | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dplayx | dxguid uuid winmm ole32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpnaddr | dpnet | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpnet | dxguid uuid ole32 advapi32 ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpnhpast | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpnhupnp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpnlobby | dpnet | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpvoice | dxguid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dpwsockx | dxguid ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| drmclien | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dsdmo | dmoguids strmiids uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dsound | dxguid uuid winmm ole32 advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dsquery | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dssenh | bcrypt crypt32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dsuiext | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dswave | dxguid uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dwmapi | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dwrite | user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | dwrite.so | — |
| dx8vb | uuid ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dxcore | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dxdiagn | strmiids dxguid d3d9 ddraw dsound version ole32 oleaut32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dxgi | gdi32 dxguid uuid wined3d user32 win32u | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dxtrans | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| dxva2 | uuid ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| esent | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| evr | mfuuid strmiids strmbase uuid dxguid ole32 oleaut32 user32 d3d9 dxva2 msvcrt | mfplat | winecrt0 msvcrt kernel32 ntdll | — | — |
| explorerframe | uuid ole32 comctl32 shell32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| faultrep | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| feclient | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fltlib | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fltmgr.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fntcache | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fontsub | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fusion | bcrypt dbghelp shlwapi version user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| fwpuclnt | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gameux | uuid shell32 oleaut32 ole32 user32 advapi32 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gamingtcui | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gdi32 | user32 advapi32 win32u | winspool | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gdiplus | uuid shlwapi ole32 oleaut32 user32 gdi32 mlang | windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| geolocation | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| glu32 | opengl32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| gphoto2.ds | comctl32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | gphoto2.so | JPEG_PE_LIBS |
| gpkcsp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| graphicscapture | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hal | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hhctrl.ocx | uuid advapi32 comctl32 shlwapi ole32 oleaut32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hid | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hidclass.sys | hal ntoskrnl user32 hidparse win32u | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hidparse.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hlink | uuid shell32 ole32 advapi32 | urlmon | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hnetcfg | oleaut32 ole32 advapi32 mpr uuid | ws2_32 winhttp shcore xmllite | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hrtfapo | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| http.sys | ntoskrnl ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| httpapi | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| hvsimanagementapi | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ia2comproxy | uuid oleacc oleaut32 ole32 rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| iccvid | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| icmp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| icmui | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ieframe | uuid urlmon shell32 comctl32 shlwapi oleaut32 ole32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ieproxy | uuid ole32 rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| iertutil | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ifsmgr.vxd | kernel | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imaadp32.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imagehlp | dbghelp | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| imm32 | user32 gdi32 advapi32 kernelbase win32u | ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| inetcomm | uuid urlmon propsys oleaut32 ole32 ws2_32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| inetcpl.cpl | comctl32 shlwapi user32 advapi32 | cryptui wininet ole32 urlmon shell32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| inetmib1 | snmpapi ws2_32 | iphlpapi | winecrt0 ucrtbase kernel32 ntdll | — | — |
| infosoft | uuid advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| initpki | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| inkobj | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| inseng | uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| iphlpapi | advapi32 dnsapi nsi uuid ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| iprop | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ir50_32 | user32 mfplat mfuuid | winegstreamer | winecrt0 ucrtbase kernel32 ntdll | — | — |
| irprops.cpl | bluetoothapis | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| itircl | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| itss | uuid urlmon shlwapi ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| joy.cpl | dxguid dinput dinput8 ole32 comctl32 user32 gdi32 advapi32 xinput | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| jscript | oleaut32 ole32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| jsproxy | uuid oleaut32 ole32 ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| kerberos | — | — | winecrt0 ucrtbase kernel32 ntdll | kerberos.so | — |
| kernel32 | kernelbase ntdll winecrt0 | — | — | — | — |
| kernelbase | uuid ntdll winecrt0 | — | — | — | — |
| ksecdd.sys | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ksproxy.ax | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ksuser | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ktmw32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| l3codeca.acm | winmm user32 kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | MPG123_PE_LIBS |
| l3codecx.ax | combase msdmo dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| light.msstyles | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| loadperf | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| localspl | spoolss user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | localspl.so | — |
| localui | winspool user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| lz32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| magnification | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mapi32 | uuid shlwapi shell32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mapistub | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mciavi32 | msvfw32 winmm user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mcicda | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mciqtz32 | strmiids oleaut32 ole32 winmm user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mciseq | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mciwave | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mf | advapi32 mfplat ole32 uuid mfuuid strmiids | evr urlmon user32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mf3216 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfasfsrcsnk | advapi32 ole32 mfplat mfuuid uuid winedmo | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mferror | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfh264enc | combase mfplat mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfmediaengine | oleaut32 ole32 mfplat mf mfuuid dxguid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfmp4srcsnk | advapi32 ole32 mfplat mfuuid uuid winedmo | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfplat | advapi32 ole32 dmoguids mfuuid propsys rtworkq kernelbase | bcrypt | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfplay | mfplat mf ole32 user32 uuid mfuuid strmiids | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfreadwrite | mfuuid uuid mfplat ole32 kernelbase | mf | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mfsrcsnk | advapi32 ole32 mfplat mfuuid uuid winedmo | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mgmtapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| midimap | winmm user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mlang | uuid ole32 gdi32 advapi32 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mmcndmgr | uuid oleaut32 ole32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mmdevapi | ole32 oleaut32 user32 advapi32 version | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mmdevldr.vxd | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| monodebg.vxd | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mouhid.sys | ntoskrnl hidparse win32u | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mountmgr.sys | uuid advapi32 ntoskrnl | user32 | winecrt0 ucrtbase kernel32 ntdll | mountmgr.so | — |
| mp3dmod | dmoguids msdmo ole32 uuid wmcodecdspuuid kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | MPG123_PE_LIBS |
| mpr | user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mprapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msacm32 | winmm user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msacm32.drv | msacm32 winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msado15 | oleaut32 ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msadp32.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msasn1 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msauddecmft | combase mfplat mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mscat32 | wintrust | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mscms | shlwapi advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | LCMS2_PE_LIBS |
| mscoree | dbghelp uuid shell32 advapi32 ole32 oleaut32 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mscorwks | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msctf | uuid ole32 oleaut32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msctfmonitor | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msctfp | rpcrt4 ole32 oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msdaps | uuid oleaut32 ole32 rpcrt4 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msdasql | uuid ole32 oleaut32 odbc32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msdelta | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msdmo | dmoguids uuid ole32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msdrm | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msftedit | uuid riched20 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msg711.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msgsm32.acm | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | GSM_PE_LIBS |
| mshtml | strmiids uuid urlmon shlwapi shell32 ole32 oleaut32 user32 gdi32 advapi32 | wininet inetcomm | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mshtml.tlb | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msi | uuid urlmon wininet comctl32 shell32 shlwapi cabinet oleaut32 ole32 version user32 gdi32 advapi32 rpcrt4 sxs imagehlp mspatcha odbccp32 wintrust crypt32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msident | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msimg32 | gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msimsg | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msimtf | uuid imm32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msisip | crypt32 ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msisys.ocx | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msls31 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msmpeg2vdec | combase mfplat mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msnet32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mspatcha | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msports | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msrle32 | winmm user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msscript.ocx | gdi32 user32 ole32 oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mssign32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mssip32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mstask | rpcrt4 ole32 oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msttsengine | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msv1_0 | netapi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | msv1_0.so | — |
| msvcirt | msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| msvcm80 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcm90 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp100 | msvcr100 | — | winecrt0 msvcr100 kernel32 ntdll | — | — |
| msvcp110 | msvcr110 | — | winecrt0 msvcr110 kernel32 ntdll | — | — |
| msvcp120 | msvcr120 | — | winecrt0 msvcr120 kernel32 ntdll | — | — |
| msvcp120_app | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp140 | vcruntime140 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp140_1 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp140_2 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp140_atomic_wait | kernelbase msvcp140 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp140_codecvt_ids | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcp60 | msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| msvcp70 | msvcr70 | — | winecrt0 msvcr70 kernel32 ntdll | — | — |
| msvcp71 | msvcr71 | — | winecrt0 msvcr71 kernel32 ntdll | — | — |
| msvcp80 | msvcr80 | — | winecrt0 msvcr80 kernel32 ntdll | — | — |
| msvcp90 | msvcr90 | — | winecrt0 msvcr90 kernel32 ntdll | — | — |
| msvcp_win | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvcr100 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr110 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr120 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr120_app | — | — | winecrt0 kernel32 ntdll | — | — |
| msvcr70 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr71 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr80 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcr90 | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcrt | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvcrt20 | msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| msvcrt40 | — | — | winecrt0 kernel32 ntdll | — | — |
| msvcrtd | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| msvfw32 | winmm version comctl32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvidc32 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msvproc | combase mfplat mfuuid dmoguids dxguid strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mswsock | ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msxml | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msxml2 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msxml3 | uuid urlmon shlwapi oleaut32 ole32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | XML2_PE_LIBS, XSLT_PE_LIBS |
| msxml4 | ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| msxml6 | oleaut32 ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| mtxdm | uuid ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ncrypt | bcrypt | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| nddeapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ndis.sys | advapi32 ntoskrnl iphlpapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| netapi32 | rpcrt4 iphlpapi ws2_32 advapi32 dnsapi netutils | — | winecrt0 ucrtbase kernel32 ntdll | netapi32.so | — |
| netcfgx | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| netio.sys | ntoskrnl ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| netprofm | iphlpapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| netutils | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| newdev | setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ninput | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| normaliz | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| npmshtml | mshtml | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| npptools | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| nsi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| nsiproxy.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | nsiproxy.so | — |
| ntdll | winecrt0 | — | — | ntdll.so | MUSL_PE_LIBS |
| ntdsapi | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ntoskrnl.exe | advapi32 hal msvcrt | rpcrt4 setupapi | winecrt0 msvcrt kernel32 ntdll | — | — |
| ntprint | winspool | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| objsel | strmiids uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| odbc32 | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | odbc32.so | — |
| odbcbcp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| odbccp32 | uuid advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| odbccu32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ole32 | uuid advapi32 user32 gdi32 combase rpcrt4 kernelbase coml2 | oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| oleacc | uuid oleaut32 ole32 user32 rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| oleaut32 | uuid ole32 rpcrt4 user32 gdi32 advapi32 | comctl32 urlmon windowscodecs | winecrt0 ucrtbase kernel32 ntdll | — | — |
| olecli32 | ole32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| oledb32 | uuid oleaut32 ole32 comctl32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| oledlg | ole32 comdlg32 user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| olepro32 | oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| olesvr32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| olethk32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| opcservices | uuid ole32 advapi32 urlmon xmllite oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| opencl | — | — | winecrt0 ucrtbase kernel32 ntdll | opencl.so | — |
| opengl32 | user32 gdi32 advapi32 win32u | glu32 | winecrt0 ucrtbase kernel32 ntdll | opengl32.so | — |
| packager | uuid shell32 shlwapi user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| pdh | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| photometadatahandler | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| pidgen | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| powrprof | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| printui | shell32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| prntvpt | winspool ole32 oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| profapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| propsys | ole32 oleaut32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| psapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| pstorec | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| pwrshplugin | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| qasf | strmbase dmoguids strmiids uuid ole32 oleaut32 wmvcore msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| qcap | strmbase strmiids uuid ole32 oleaut32 winmm msvcrt | msvfw32 | winecrt0 msvcrt kernel32 ntdll | qcap.so | — |
| qdvd | strmbase strmiids uuid ole32 msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| qedit | strmiids strmbase uuid oleaut32 ole32 advapi32 msvcrt | — | winecrt0 msvcrt kernel32 ntdll | — | — |
| qmgr | uuid winhttp ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| qmgrprxy | rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| quartz | strmiids dxguid strmbase uuid dsound msacm32 msvfw32 ole32 oleaut32 rpcrt4 user32 gdi32 advapi32 winmm msvcrt | ddraw | winecrt0 msvcrt kernel32 ntdll | — | — |
| query | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| qwave | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rasapi32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rasdlg | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| regapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| resampledmo | combase mfplat msdmo mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| resutils | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| riched20 | uuid ole32 oleaut32 imm32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| riched32 | riched20 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rometadata | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rpcrt4 | uuid advapi32 | iphlpapi wininet secur32 user32 ws2_32 ole32 oleaut32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rsabase | rsaenh | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rsaenh | bcrypt crypt32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rstrtmgr | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rtutils | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| rtworkq | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| samlib | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sane.ds | comctl32 user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | sane.so | — |
| sapi | uuid ole32 oleaut32 user32 advapi32 | winmm | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sas | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| scarddlg | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| scardsvr | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sccbase | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| schannel | secur32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| schedsvc | rpcrt4 advapi32 ole32 xmllite | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| scrobj | uuid urlmon ole32 oleaut32 xmllite advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| scrrun | uuid oleaut32 version advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| scsiport.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sechost | kernelbase | rpcrt4 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| secur32 | advapi32 | crypt32 | winecrt0 ucrtbase kernel32 ntdll | secur32.so | — |
| security | secur32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sensapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| serialui | user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| setupapi | uuid advapi32 rpcrt4 kernelbase | cabinet shell32 wintrust ole32 comdlg32 user32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sfc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sfc_os | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shcore | advapi32 | user32 ole32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shdoclc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shdocvw | uuid shlwapi advapi32 | version ole32 oleaut32 ieframe | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shell32 | uuid shlwapi user32 gdi32 advapi32 | ole32 oleaut32 shdocvw version comctl32 comdlg32 gdiplus | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shfolder | shell32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| shlwapi | uuid shcore user32 gdi32 advapi32 kernelbase | userenv oleaut32 ole32 comctl32 comdlg32 mpr mlang urlmon shell32 winmm | winecrt0 ucrtbase kernel32 ntdll | — | — |
| slbcsp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| slc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| snmpapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| softpub | wintrust | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| spoolss | winspool | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sppc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| srclient | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| srvcli | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| srvsvc | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sspicli | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| stdole2.tlb | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| stdole32.tlb | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sti | uuid ole32 oleaut32 rpcrt4 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| strmdll | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| svrapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| sxs | oleaut32 ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| t2embed | gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| tapi32 | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| taskschd | oleaut32 ole32 advapi32 xmllite rpcrt4 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| tbs | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| tdh | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| tdi.sys | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| threadpoolwinrt | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| traffic | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| twain_32 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| twinapi.appcore | combase advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| tzres | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| ucrtbase | ntdll | advapi32 user32 | winecrt0 kernel32 ntdll | — | MUSL_PE_LIBS |
| uianimation | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| uiautomationcore | uuid ole32 oleaut32 user32 oleacc | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| uiribbon | uuid ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| unicows | avicap32 msvfw32 oledlg comdlg32 shell32 winmm winspool rasapi32 secur32 sensapi version oleacc user32 mpr gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| updspapi | setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| url | shell32 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| urlmon | uuid ole32 oleaut32 shell32 rpcrt4 shlwapi wininet user32 advapi32 | advpack | winecrt0 ucrtbase kernel32 ntdll | — | — |
| usbd.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| user32 | gdi32 sechost advapi32 kernelbase win32u uuid | imm32 combase | winecrt0 ucrtbase kernel32 ntdll | — | PNG_PE_LIBS |
| userenv | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| usp10 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| utildll | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| uxtheme | user32 gdi32 advapi32 | msimg32 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vbscript | oleaut32 ole32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp100 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp110 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp120 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp140 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcomp90 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcruntime140 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vcruntime140_1 | vcruntime140 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vdhcp.vxd | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vdmdbg | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| version | kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vga | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| virtdisk | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vmm.vxd | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vnbt.vxd | iphlpapi ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vnetbios.vxd | netapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vssapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vtdapi.vxd | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vulkan-1 | user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| vwin32.vxd | kernel | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| w32skrnl | kernel | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wbemdisp | oleaut32 ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wbemprox | winspool version iphlpapi dxgi oleaut32 ole32 advapi32 user32 gdi32 ws2_32 rpcrt4 setupapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wdscore | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| webservices | winhttp rpcrt4 user32 ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| websocket | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wer | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wevtapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wevtsvc | rpcrt4 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wiaservc | uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wimgapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| win32u | ntdll winecrt0 | — | — | win32u.so | — |
| windows.applicationmodel | combase kernelbase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.devices.bluetooth | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.devices.enumeration | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.devices.usb | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.gaming.input | combase uuid user32 dinput8 setupapi hid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.gaming.ui.gamebar | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.globalization | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.media | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.media.devices | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.media.mediacontrol | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.media.speech | combase uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.networking | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.networking.connectivity | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.networking.hostname | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.perception.stub | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.security.authentication.onlineid | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.security.credentials.ui.userconsentverifier | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.storage.applicationdata | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.system.profile.systemmanufacturers | combase oleaut32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.ui | combase advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windows.web | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| windowscodecs | windowscodecs uuid ole32 oleaut32 propsys rpcrt4 shlwapi user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | JPEG_PE_LIBS, PNG_PE_LIBS, TIFF_PE_LIBS |
| windowscodecsext | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winealsa | uuid ole32 advapi32 | winmm | winecrt0 ucrtbase kernel32 ntdll | winealsa.so | — |
| wineandroid | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | wineandroid.so | — |
| winebth.sys | ntoskrnl uuid | — | winecrt0 ucrtbase kernel32 ntdll | winebth.so | — |
| winebus.sys | ntoskrnl hidparse | — | winecrt0 ucrtbase kernel32 ntdll | winebus.so | — |
| winecoreaudio | uuid ole32 user32 advapi32 | winmm | winecrt0 ucrtbase kernel32 ntdll | winecoreaudio.so | — |
| winecrt0 | — | — | — | — | — |
| wined3d | dxguid opengl32 user32 gdi32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | VKD3D_PE_LIBS |
| winedmo | mfuuid | — | winecrt0 ucrtbase kernel32 ntdll | winedmo.so | — |
| winegstreamer | strmbase ole32 oleaut32 msdmo msvcrt user32 | mfplat mf | winecrt0 msvcrt kernel32 ntdll | winegstreamer.so | — |
| winehid.sys | hidclass ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winemac | uuid rpcrt4 user32 gdi32 win32u | ole32 shell32 imm32 | winecrt0 ucrtbase kernel32 ntdll | winemac.so | — |
| winemapi | shlwapi shell32 mapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wineoss | uuid ole32 user32 advapi32 | winmm | winecrt0 ucrtbase kernel32 ntdll | wineoss.so | — |
| wineps | user32 gdi32 winspool advapi32 win32u | — | winecrt0 ucrtbase kernel32 ntdll | wineps.so | — |
| winepulse | dxguid uuid winmm user32 advapi32 ole32 | — | winecrt0 ucrtbase kernel32 ntdll | winepulse.so | — |
| wineusb.sys | ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | wineusb.so | — |
| winevulkan | user32 gdi32 advapi32 setupapi win32u | — | winecrt0 ucrtbase kernel32 ntdll | winevulkan.so | — |
| winewayland | — | — | winecrt0 ucrtbase kernel32 ntdll | winewayland.so | — |
| winex11 | uuid user32 gdi32 win32u | — | winecrt0 ucrtbase kernel32 ntdll | winex11.so | — |
| winexinput.sys | ntoskrnl hidparse | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wing32 | user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winhttp | uuid jsproxy user32 advapi32 ws2_32 | oleaut32 crypt32 secur32 iphlpapi dhcpcsvc | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wininet | mpr shlwapi shell32 user32 ws2_32 advapi32 | secur32 crypt32 cryptui dhcpcsvc iphlpapi | winecrt0 ucrtbase kernel32 ntdll | — | ZLIB_PE_LIBS |
| winmm | uuid user32 advapi32 ole32 msacm32 | dinput8 | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winnls32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winprint | winspool | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winscard | — | — | winecrt0 ucrtbase kernel32 ntdll | winscard.so | — |
| winspool | user32 gdi32 advapi32 compstui | — | winecrt0 ucrtbase kernel32 ntdll | winspool.so | — |
| winsta | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wintab32 | user32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wintrust | crypt32 user32 advapi32 | cryptui imagehlp | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wintypes | combase | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| winusb | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wlanapi | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wlanui | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wldap32 | crypt32 secur32 ws2_32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | LDAP_PE_LIBS |
| wldp | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmadmod | combase mfplat msdmo mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmasf | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmi | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmilib.sys | hal ntoskrnl | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmiutils | oleaut32 ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmp | oleaut32 ole32 urlmon shlwapi user32 gdi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmphoto | windowscodecs uuid ole32 oleaut32 propsys rpcrt4 shlwapi | — | winecrt0 ucrtbase kernel32 ntdll | — | JXR_PE_LIBS |
| wmvcore | kernel32 | winegstreamer | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wmvdecod | combase mfplat msdmo mfuuid dmoguids strmiids wmcodecdspuuid uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wnaspi32 | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wofutil | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wow32 | kernel | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wow64 | ntdll winecrt0 | — | — | — | — |
| wow64cpu | wow64 ntdll winecrt0 | — | — | — | — |
| wow64win | wow64 win32u ntdll winecrt0 | — | — | — | — |
| wpc | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wpcap | iphlpapi | ws2_32 | winecrt0 ucrtbase kernel32 ntdll | wpcap.so | — |
| ws2_32 | — | dnsapi advapi32 iphlpapi user32 | winecrt0 ucrtbase kernel32 ntdll | ws2_32.so | — |
| wsdapi | bcrypt iphlpapi rpcrt4 user32 webservices ws2_32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wshom.ocx | uuid oleaut32 ole32 shell32 user32 advapi32 scrrun | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wsnmp32 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wsock32 | mswsock ws2_32 iphlpapi | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wtsapi32 | advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wuapi | uuid ole32 advapi32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| wuaueng | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| x3daudio1_0 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_1 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_2 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_3 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_4 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_5 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_6 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| x3daudio1_7 | — | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine2_0 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine2_4 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine2_7 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine2_9 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_0 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_1 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_2 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_3 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_4 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_5 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_6 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xactengine3_7 | ole32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xapofx1_1 | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xapofx1_2 | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xapofx1_3 | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xapofx1_4 | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xapofx1_5 | ole32 | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_0 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_1 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_2 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_3 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_4 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_5 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_6 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_7 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_8 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xaudio2_9 | advapi32 ole32 user32 uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | FAUDIO_PE_LIBS |
| xinput1_1 | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xinput1_2 | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xinput1_3 | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xinput1_4 | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xinput9_1_0 | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xinputuap | hid setupapi advapi32 user32 | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xmllite | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xolehlp | uuid | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xpsprint | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xpssvcs | — | — | winecrt0 ucrtbase kernel32 ntdll | — | — |
| xtajit64 | ntdll winecrt0 | — | — | — | — |

<!-- END GENERATED WINE AUDIT -->

## Evidence boundary and dependency decisions

This deliverable exercises the directive's pending-download fallback. The
archive is not verified locally. **Do not freeze the core ABI from this audit.**
The tagged source inventory has 665 files; the offline snapshots include 73
module `.spec` files and 74 build Makefiles. All 27 requested/candidate/P4/P5
modules are covered. The root census reads 387 declared C/C++/IDL and local
header inputs, records 60 runtime loader/lookup source sites and 52 matching
literal observations, and preserves 461 explicit `.spec` forwards and 413
`-import` export entries. Literal observations may repeat by source location.
All named Makefile dependencies in this graph resolve; configure-variable
libraries, COM activation, runtime-selected drivers, child executables and
unexpanded IDL/header code remain additional closure work.

The source references intersect the pinned **i386 export catalogs**; source
files are not preprocessed for i386. Architecture guards, dormant functions,
local declarations, aliases and alternative providers can change the actual
imports. `-arch=win64` exports are excluded, explicit ordinals are retained,
automatic ordinals are left unassigned, and stubs/private flags stay visible.
The JSON includes first source locations and full forward/`-import` targets.
The host test checks names against these original `.spec` snapshots and hashes;
it does not prove that an exported stub implements useful behavior.

The four-core source union is **1,052 names**: NTDLL 242, KERNEL32 374,
USER32 301, GDI32 135. Excluding the six full core/candidate modules
`advapi32`, `winmm`, `dinput`, `ddraw`, `dsound`, `msvcrt` still leaves
**784** P4/P5 source names (86 + 280 + 287 + 131). The modern provider
columns overlap that set: e.g. KERNEL32 and KERNELBASE export many identical
names. They cannot be added as independent function counts. Neither number
is an exact linked requirement or a verified reachable subset.

The 40-function warning in the original contract does not qualify an ABI.
The verified small DLL alone has **35 imports and 2 forwarded requirements**:
9 KERNEL32 exports including the two forward targets, 3 NTDLL debug exports,
21 KERNELBASE exports and 4 UCRTBASE exports. USER32 and GDI32 are absent in
this particular PE. These are exact names for the recorded experiment hash,
not the dependency list of a future canonical Wine build or a game workload.
The three NTDLL imports are Wine debug ABI; the experiment does not import
`__wine_unix_call_dispatcher` or `__wine_unixlib_handle`.

### Server, Unix and private boundaries

No direct `SERVER_START_REQ`, `wine_server_call`, `__wine_unix_call` or
`WINE_UNIX_CALL` site was observed in the audited root source inputs.
This is consistent with implementing the core services in CiukiOS rather
than porting Wine's Unix backends. It does not make upstream core DLLs
freestanding: [NTDLL's Makefile](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/ntdll/Makefile.in)
and [WIN32U's Makefile](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/win32u/Makefile.in)
define Unix companions; the graph also reaches Unix-backed crypto/network/
graphics modules. Reusing those core implementations would introduce their
host and wineserver boundary.

The three-file NTDLL Unix survey names the actual requests used there, including
`create_file`, `create_named_pipe`, `get_handle_fd`, `close_handle`, `select`,
`queue_apc`, event/mutex/semaphore operations, mappings, completion and directory
operations. Its table deliberately names the surveyed files rather than assigning
all their requests to every root DLL. A complete per-root reachable wineserver
protocol cannot be established from this source scan. `NtCreateFile` reaches
the `create_file` helper, `NtCreateNamedPipeFile` sends `create_named_pipe`,
and `NtClose` sends `close_handle` in the pinned sources. CiukiOS option C
needs corresponding semantics, not these exact Wine wire requests.

The common-helper survey records NTDLL debug functions (five header candidates;
three present in the built PE), Wine exception helpers, DLL registration helpers,
delay-load helpers and Unix-dispatch state. `__wine_register_resources` and
`__wine_unregister_resources` are static winecrt0 helpers, not proof of missing
NTDLL exports. Their generated data, registry calls and IDL output must be audited
when the larger DLLs are linked. `__wine_unix_call_dispatcher` and
`__wine_unixlib_handle` are private NTDLL data exports in the pinned `.spec`;
their presence in a generic helper header does not mean every root imports them.

Two concrete runtime boundaries escape an IMPORTS-only graph:

- [WinMM waveform](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/winmm/waveform.c)
  activates `CLSID_MMDeviceEnumerator`; MMDevAPI/audio endpoint services need
  a deliberate adapter or a retained Ciuki WinMM core. This class is not a
  literal `mmdevapi.dll` Makefile import.
- [COMBASE RPC](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/dlls/combase/rpc.c)
  has RPCSS startup and local-server/surrogate creation paths. An in-process
  COM subset requires an explicit reachable-path cut and tests; dropping
  out-of-process COM cannot be inferred just from the installer-facing API list.

### PE-only feasibility, notices and expected patches

"PE-only" below means the module can be compiled into PE code; it does not
mean its backend/registry/classes/driver dependencies exist in CiukiOS. No
source patch was applied in this experiment. Subset patches below are proposed
decision input, not implemented changes or success-returning substitutes.
Every unbuilt row still needs direct/delay/forward/ordinal/private/runtime PE
closure inspection before it can become an approved reuse manifest.

| DLL | PE-only build evidence or constraint | Required notices | Expected adapter/patch decision |
| --- | --- | --- | --- |
| version | Built i386 PE32, 16 exports, VERSIONINFO resource, 35 imports | Wine LICENSE, COPYING.LIB, AUTHORS and source headers; winecrt0 included | Supply KERNELBASE and UCRTBASE facade/imports, plus debug ABI; canonical toolchain comparison pending |
| shell32 | Not built; PE sources with large direct/delay/forward closure | Wine notices and original resource notices | Replace or cut Unix recycle-bin/path conversions, process helpers and unsupported shell namespace paths; preserve installer operations |
| comctl32 | Not built; kernelbase/imm32 plus delayed uxtheme/winmm | Wine notices and resource notices | Decide v5/v6/theme/IME coverage; complete USER/GDI semantics; no blanket removal of controls used by installers |
| comdlg32 | Not built; shell32/comctl32/winspool, delayed ole32 | Wine notices and resource notices | Decide print/modern item-dialog paths and COM classes; retain measured installer file dialogs |
| ole32 | Not built; 134 explicit forwards, COMBASE/RPCRT4/COML2 boundary | Wine notices | Resolve COM apartment/marshal/storage boundary and RPCSS/out-of-process paths before freezing exports |
| oleaut32 | Not built; RPCRT4/COM, delayed URLMON/WINDOWSCODECS | Wine notices | Decide automation/type-library/picture paths, generated proxies and supported image decoders |
| rpcrt4 | Full module audited; subset not specified; networking/security delay libraries | Wine notices | Define an in-process marshaling subset with actual generated proxy use; networking/authentication removal needs evidence |
| msvcrt | Full module source audited; custom exception ABI and MUSL_PE_LIBS | Wine plus musl MIT and file-specific notices | Specify which CRT versions games need; do not confuse game MSVCRT with Wine upper DLLs' UCRTBASE |
| setupapi | Not built; RPCRT4/KERNELBASE, cabinet/shell/trust/UI delays | Wine notices | Limit to installer INF/file operations with recorded hardware/trust paths; no unsupported driver-install success |
| advapi32 | Full module audited, including 196 imported exports; KERNELBASE/SECHOST/MSVCRT | Wine notices | Retain Ciuki registry/security core or specify precisely which Wine parts survive service/LSA/crypto/eventlog cuts |
| winmm | Full module audited; MMDevice COM activation and delayed dinput8 | Wine notices | Keep Ciuki audio/input/MCI boundary; select portable parts/codecs without silently importing a modern audio host |
| dinput | Candidate; PE code with HID/setupapi/USER/raw-input dependency | Wine notices | Retain approved Ciuki input core unless a concrete HID/raw-input adapter beats it |
| ddraw | Candidate; PE code imports WineD3D, which reaches OpenGL/Vulkan paths | Wine notices plus selected graphics dependencies' notices | Retain F5 Ciuki software DirectDraw; full WineD3D reuse is outside the small offline F5 boundary |
| dsound | Candidate; PE code and COM/audio backend requirements | Wine notices | Retain Ciuki sound adapter or prove a supported endpoint implementation |
| shlwapi | Not built; SHCORE/KERNELBASE and many forwards/imported exports | Wine notices | Define string/path/registry/shell subset and native/builtin precedence; adapt path semantics |
| riched20 | Not built; OLE/OLEAUT/IME/USER/GDI | Wine notices | Define rich text/OLE/font behavior; generated TOM IDL and font residency matter |
| riched32 | Not built; wrapper around riched20/USER | Wine notices | No independent text engine; include riched20 closure |
| cabinet | Not built; ZLIB_PE_LIBS configured/bundled source | Wine plus bundled zlib 1.3.1 notice | Pin compression backend; handle CAB callbacks/errors rather than replacing the installer |
| advpack | Not built; setupapi/version/advapi/ole32 | Wine notices | INF/registration/restart flow subset; cannot be qualified before setupapi/shell/core closure |
| lz32 | Data-only PE forwarding DLL, not linked in this experiment | Wine notices | Resolve its 12 forwards; do not budget an independent decompressor from this module name |
| msacm32 | Not built; portable PCM converter plus driver/WinMM boundary | Wine notices | Map ACM driver loading and buffers to Ciuki audio; no Unix driver DLLs in the reuse manifest |
| msadp32.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Codec host fixtures and callback/heap checks; no backend chosen by this audit |
| imaadp32.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Same boundary validation for IMA ADPCM |
| msg711.acm | Not built; portable codec candidate, WinMM/USER callbacks | Wine notices | Same boundary validation for G.711; needed format remains census-dependent |
| msvfw32 | Not built; WinMM/version/comctl32/USER/GDI | Wine notices | MCI/codec and window-message integration; driver discovery subset |
| msrle32 | Not built; portable codec candidate, WinMM/USER | Wine notices | RLE codec production tests plus driver callbacks |
| msvidc32 | Not built; portable codec candidate, USER | Wine notices | Video 1 codec production tests plus driver callbacks |

Wine's [LICENSE](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/LICENSE)
and [COPYING.LIB](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/COPYING.LIB)
govern the reused Wine code. Retain per-file copyright/alternate-license
notices, AUTHORS, complete corresponding source and the exact build/patch
manifest; retain the LGPL replacement/relinking arrangement required by the
selected distribution method. A PE boundary alone does not establish that
arrangement. The fixture snapshots are audit inputs, not corresponding source
for shipping a DLL. The [musl COPYRIGHT](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/musl/COPYRIGHT)
and [bundled zlib header](https://github.com/wine-mirror/wine/blob/b073859675060c9211fcbccfd90e4e87520dc2c2/libs/zlib/zlib.h)
are retained separately; any other selected PE libraries need their own pinned
notices rather than being relabeled LGPL.

## Build experiment and lead recipe

The host has clang/lld **23.1.1**, Wine tools **11.18**, and no MinGW GCC.
The latter was an assumption in earlier directives, not a verified absence
of all Wine tools. Neither system winegcc nor system winebuild was used to
compile/link the DLL. The Wine **10.0** tools were built from the pinned
source with clang using a small Linux configuration header. The PE code uses
Wine 10.0 Windows and CRT headers, not system Wine 11.18 headers. The target
is `i686-windows`, `-march=pentiumpro -mno-sse -mno-sse2`, with `__WINESRC__`,
`__WINE_PE_BUILD`, `_UCRT`, `-fno-builtin` and `-fshort-wchar`.

`winebuild --implib` creates pinned import libraries for KERNELBASE, UCRTBASE,
KERNEL32 and NTDLL. The recipe builds the winecrt0 debug/default-DllMain/CRT
entry objects and uses winegcc `-b i686-windows -mno-cygwin -shared`,
`-Wl,--wine-builtin` (winebuild `--builtin`) and `-Wl,-timestamp:0`.
The COFF symbol/section headers and imports were independently inspected with
llvm-readobj; the production Ciuki census parser agrees on every import.
Two consecutive script runs produced identical hashes for all three artifacts.

| Output under build/host/win32/wine-10.0 | SHA-256 |
| --- | --- |
| tools/winebuild | `e6cf212c382f95af8d8b1cbe2c33bde311c948343e765192b9ae0baacf11ebc9` |
| tools/winegcc | `fffa94b2fde1fb6198f19bb6146d5cfdb07905bb5e0bf51cddc212426768930c` |
| version.dll | `f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32` |

The DLL is 40,960 file bytes, 32,768 SizeOfImage bytes, with a 948-byte resource
directory and relocations. Its OS/subsystem header version is 6.0 from the modern
toolchain; no Windows 98 or Ciuki runtime result is implied. The export resource
and entry point are present. The bootstrap configuration is a build experiment,
not an upstream configure qualification or the canonical F5 distribution recipe.

Exact encountered gaps/failures and fixes:

1. WineHQ archive: sandbox DNS failure, still pending. Individual tagged files
   supply this limited experiment but not the complete generated release tree.
2. winegcc bootstrap: `CXX`/`CPP` configuration macros missing on the first
   attempt; adding the normal tool configuration macros fixed compilation.
3. PE source: missing Wine header dependencies (`sal.h`, `concurrencysal.h`)
   were fetched from the same commit; `TRACE`/`WARN` required `__WINESRC__`.
   Neither mingw-w64 headers nor an external MinGW CRT was needed for this DLL.
4. LLVM resource compiler: rejected Wine's adjacent string literals in the
   VERSIONINFO BLOCK (`expected '{', got "04B0"`). The separately hash-pinned
   system **wrc 11.18** parses the original resource using `--nostdinc` and only
   pinned Wine headers. Rebuild with Wine 10.0 wrc during canonical qualification;
   do not claim the entire experiment's toolchain is Wine 10.0.
5. Larger DLLs: generated IDL/proxy/registration headers, complete host tools,
   and configured target libraries are not built here. `ZLIB_PE_LIBS`,
   `MUSL_PE_LIBS`, graphics/crypto/font/media library variables still require
   explicit configured values and hashes.

Repeat the implemented recipe, with all jobs sequential and at nice 19:

```sh
nice -n 19 bash scripts/win32/build-wine-dlls.sh
nice -n 19 python3 scripts/win32/wine_audit.py check
nice -n 19 python3 -m unittest discover -s tests/host/win32 -p test_wine_audit.py -v
```

It verifies all source/tool hashes before building, writes `hashes.json`,
`version-pe.json` and `version-readobj.txt`, and never runs Wine or a guest.
The source tree may be replaced by the verified release archive after the
lead checks its pinned SHA-256; the source-file verifier must still pass.
For the upstream build route, Wine 10.0 configure accepts
`--enable-archs=i386 --with-mingw=clang`, with `CC=clang`, `i386_CC=clang` and
the recorded i386 CPU flags. Build tools and selected `dlls/<module>/all`
targets with `make -j1`, then inspect actual PE outputs. The lead runs any
larger build in the repository's capped scope. If an external MinGW route is
needed, `config/wine-pin.json` supplies the optional **mingw-w64 12.0.0** archive
and its [MXE-pinned SHA-256](https://github.com/mxe/mxe/blob/29894c0c51b378030478e51530f7f558a6e9430f/src/mingw-w64.mk);
headers and CRT must both be configured for the same runtime. No missing
external CRT is asserted for the already successful version experiment.

## Core size versus the ten-title census

No f5-02 title/installer/installed-binary import census is present in this
worktree. The available `scripts/pe_import_census.py` is a driver census/parser,
not measurements of the ten game titles. Thus the title-required four-core
export count and the incremental Wine-reuse cost are **unknown**, not zero.

| Approved title | Installer + installed/child/runtime DLL → function census |
| --- | --- |
| Diablo shareware | Pending f5-02A/B |
| StarCraft shareware | Pending f5-02A/B |
| Age of Empires trial | Pending f5-02A/B |
| Half-Life: Uplink | Pending f5-02A/B |
| Unreal Tournament demo 348 | Pending f5-02A/B |
| Worms Armageddon demo | Pending f5-02A/B |
| Heroes of Might and Magic III demo | Pending f5-02A/B |
| RollerCoaster Tycoon demo | Pending f5-02A/B |
| Jazz Jackrabbit 2 shareware | Pending f5-02A/B |
| Total Annihilation demo | Pending f5-02A/B |

The f5-02 format is DLL → named/ordinal function list per hashed PE, keeping
direct, delay and later runtime observations distinct. The build's verified
KERNEL32 list, for example, is:

```text
Wine 10.0 version.dll / SHA-256 f4ccd4292b9b9e40af081f9479c15d759256c70898cbd952666ce1e3bce12d32
KERNEL32.DLL -> IsBadStringPtrA, LZClose, LZCopy, LZOpenFileA, MoveFileA, OpenFile, _lclose
forward targets -> kernel32.VerLanguageNameA, kernel32.VerLanguageNameW
```

Once the real census and full linked reuse manifest exist, compute a set union
per provider and the difference `reused_exports - title_exports`. Do not
subtract the unpreprocessed 1,052-name source candidate count from a static
game census and call the result an implemented ABI requirement.

## 128 MiB footprint and recommendation

Only Wine 10.0 **version.dll** has a measured PE size here. Its 32 KiB image
is small, but its 1 MiB heap-reserve / 4 KiB heap-commit header fields do not
create a heap for this DLL: it calls the process heap. Summing those fields
across DLLs would invent allocations. Heap use is input/path dependent;
the source initially allocates 1,000 bytes per version-information buffer and
can grow to a WORD-sized length, with two buffers during comparison and
additional filename conversions. No runtime heap high-water mark was measured.

To supply a numerical **planning proxy**, the installed Wine **11.18 i386**
copies of the same 27 modules were read without execution. Their names/hashes,
file sizes, SizeOfImage and heap header values are in `memory_proxy`. They total
**19,588,512 file bytes (18.681 MiB)** and **47,218,688 SizeOfImage bytes
(45.031 MiB)**. These are not Wine 10.0 measurements or runtime residency;
file bytes include nonresident data/symbols, and SizeOfImage includes alignment,
zero-fill and reservation. Shared read-only pages are charged once, while
private writable pages and heaps depend on each process and touched paths.

| Planning component | MiB | Basis |
| --- | ---: | --- |
| All 27 upper/candidate images fully resident | 45.031 | Wine 11.18 SizeOfImage stress proxy, not a measured Wine 10.0 working set |
| Native kernel/desktop/reservations | 32 | Assumption; actual supported-image baseline pending |
| Ciuki core/facades | 8 | Assumption; code not implemented/sized |
| Win32 private heaps | 8 | Assumption per active workload, not PE heap header sum |
| Surfaces/audio/sample cache | 8 | Assumption; title and GM-bank measurements pending |
| One active title/installer payload | 24 | Assumption; ten-title peak census pending |
| Total stress scenario | **125.031** | Only **2.969 MiB** of 128 MiB remains before extra processes/closure/queues/stacks/COW |

This illustrates why an all-loaded unmodified manifest is unsafe as a fit
claim; it is not an estimate of actual simultaneous DLL loading. The 18.681
MiB file proxy also cannot be substituted as committed RAM. Demand paging,
sharing, optional path cuts and sparse heaps can lower residency; helper
chains, extra core/COM/crypto/driver DLLs, queues, synth data and surfaces can
raise it. The 27-module proxy excludes the full 74-node graph, its Unix
companions and child programs. A full Wine port's memory is likewise unmeasured.
No hardware reservations or memory limits were changed by this audit.

**Recommendation to the lead:** retain approved option C provisionally and
hold the ABI freeze. The successful 37-requirement version slice shows leaf
reuse is technically feasible without a complete POSIX host. The untrimmed
P4/P5 source boundary (784 names even without the six core/candidate modules),
modern provider/CRT dependencies, shell Unix helpers and COM activation paths
do not establish a small or cheap boundary. A numerical claim that C still
beats a full Wine/POSIX port is not supported yet.

The lead must choose the exact installer-visible subset and KERNELBASE/UCRTBASE/
COMBASE/COML2/SECHOST/SHCORE facade policy, merge the f5-02 census, and compare
actual linked sizes/peak committed memory of one installer vertical slice.
Keeping the approved Ciuki DirectDraw/input/sound/registry core avoids the
candidate backends identified above, but does not settle upper UI/COM semantics.
If the required subset still entails effectively full modern core/COM behavior,
return option B as a separate reviewed POSIX/host-adaptation proposal to the
owner; do not silently change Q2 or declare C defeated by source name counts.

## Host validation

`tests/host/win32/test_wine_audit.py` runs offline and does not rebuild. It
checks every named core/modern candidate and actual PE import against pinned
`.spec` files, counts/unions, architecture filtering, forwards/import exports,
Makefile graph/default/delay consistency, protocol tokens, notices and generated
Markdown. Negative mutations reject invented exports, changed pins/counts/graphs
and corrupted snapshots. When the optional pinned source cache or built PE
exists, it independently reproduces the source census and compares PE imports
with llvm-readobj. Those live checks skip on a fresh checkout without build
artifacts; all offline snapshot checks still run.

The existing `tests/host/test_pe_import_census.py` suite separately exercises
the production import parser with synthetic named/ordinal/delay/truncated
PE/INF inputs. Neither suite proves a Ciuki DLL load, API fidelity, actual
title imports, hardware behavior, complete reachable protocol, licence release
arrangements or a 128 MiB runtime fit. Those remain lead integration evidence.

### Milestone C measured implementation decision

The P5 PE images are original bounded LGPL-2.1-or-later implementations.
The unmodified Wine paths are not selected: their default UCRT/modern-provider
closure has no qualified CiukiOS backend, and the pinned Cinepak reader has
unchecked global-cursor/chunk reads. This is a dependency/safety decision,
not a claim that an unbuilt Wine DLL exceeded the memory budget. No Wine
candidate file size or Wine resident measurement is available without the
staged release tree. The lead must review this alternative under f5-14C's
Wine-versus-native decision rule. No new core exports are introduced.

The table describes `make CAP= apps`'s O1 store build. Standalone host
reproducibility builds use the recipe's O2 flags and their own fixed link bases;
their hashes are recorded separately and are not the image's hashes.

| Module | Decision | File bytes | SizeOfImage | Shared seed pages | Private COW credits | Cold resident pages |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| msacm32.dll | bounded native | 10240 | 61440 | 6 | 9 | 15 |
| imaadp32.acm | bounded native | 8704 | 61440 | 6 | 9 | 15 |
| msadp32.acm | bounded native | 8704 | 61440 | 6 | 9 | 15 |
| msvfw32.dll | bounded native | 11776 | 28672 | 6 | 1 | 7 |
| avifil32.dll | bounded native | 12288 | 28672 | 6 | 1 | 7 |
| msrle32.dll | bounded native | 10240 | 28672 | 6 | 1 | 7 |
| msvidc32.dll | bounded native | 10240 | 28672 | 6 | 1 | 7 |
| iccvid.dll | bounded native | 10240 | 28672 | 6 | 1 | 7 |

Totals: 82,432 file bytes, 327,680 image bytes, 80 cold pages (320 KiB).
This is 0.2441% of 128 MiB, excluding core DLLs, page tables, loader metadata
and active stream/frame allocations. The model follows
`src/kernel/proc/win32/sections.c`: `image_seed` eagerly seeds the union of
header/raw pages; `map_view` preallocates writable-page COW credits. Image
reservation is separate; BSS backing is demand-created. These are calculated
cold-store figures, not guest RSS or a full-system 128 MiB qualification.

| DLLs | Exact imports | Missing |
| --- | --- | --- |
| msacm32, imaadp32.acm, msadp32.acm | none | none |
| msvfw32, msrle32, msvidc32, iccvid | kernel32: GetProcessHeap, HeapAlloc, HeapFree | none |
| avifil32 | kernel32: CloseHandle, CreateFileA, GetFileSize, GetProcessHeap, HeapAlloc, HeapFree, ReadFile, SetFilePointer, WideCharToMultiByte; msvfw32: ICClose, ICDecompress, ICLocate, ICSendMessage | none |

The initially missing `kernel32!CreateFileW` was removed from the native
consumer: AVIFileOpenW uses existing WideCharToMultiByte with no-best-fit and
checks substitution before CreateFileA. Non-ACP paths fail rather than open a
different filename. No Wine-private export, Unix library, wineserver or WinMM
private API is imported. T1 checks the exact P5 set, PE hashes, license/origin,
source/notices hashes and import closure against actual provider PE exports.

All eight images export the pinned spec names (and explicit ordinals). Their
machine-readable coverage distinguishes implemented functions/constants from
clean unsupported entries. Unknown historical stub conventions do not become
compatibility claims. Synchronous decode and read-only AVI are implemented;
filters, chooser/UI, encoding, arbitrary installable drivers, AVI editing/COM,
OpenDML and MCI window integration are not supplied by this subset. See
`tests/host/win32/codecs/README.md` for format/output and resource boundaries.
The full upstream Wine programs are not claimed as passing; selected portable
assertions are run on the native adapter and rebuilt as PE. H1 PE execution is
skipped on the adapter's SIGSYS/seccomp restriction. QEMU/T23 remain unrun.

### Milestone C host validation result

Final Win32 discovery: 118 tests, 107 passed and 11 environment skips, no
failures. The codec group covers exact authored ADPCM output, pinned frame
digests, DriverProc and i386 layouts, 24,000 ASan/UBSan mutations, and actual
production-loader H0 mapping/binding/unload. Selected upstream assertions
pass natively and compile as PE; SIGSYS blocks H1 execution in this sandbox.
The other skips are existing i386/Wine socket restrictions and unavailable
optional milestone-A caches. `make CAP= apps` and API-reference regeneration/
check pass. Both consecutive standalone recipe runs have identical hashes;
canonical O1 records match an independently rebuilt H0 store for all eight
DLLs. The 28-entry canonical inventory passes T1 and has no missing P5 imports.

Detailed hashes, notices, coverage and qualification limits are recorded in
`build/codex/f5-14c-report.md` and `build/codex/f5-14c-evidence.json` for lead
review. Guest probes, QEMU, T23, native-substitution approval and wider API/
thread conformance remain distinct gates. No image or guest evidence was
produced.
