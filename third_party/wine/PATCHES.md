# P5 patches

No patch is applied to a Wine tree by the P5 recipe. The codec implementations
are maintained as normal source in `sdk/win32/codecs/`; they do not masquerade
as unmodified Wine DLLs. Interface metadata in `codecs/` is unmodified.
P4 patches belong to the parallel f5-14B implementation.

F5-14B delivery: no Wine source patch is applied and no reused P4 PE is
distributed. In-process COM/RPC transport changes remain unimplemented;
there are no invented or unapplied `.patch` files. DirectSetup is original
CiukiOS source in `sdk/win32/upper/`, built directly rather than patched into
Wine. The complete-source build prerequisite and remaining patches are
recorded in `docs/design/win32-wine-dependency-audit.md` and the B report.

## F5-14B continuation (2026-10-19)

The earlier no-patch statement describes the partial delivery. The offline
`--upper-build` recipe now applies these real patches, in order, with
`patch --fuzz=0 -p1`, to a fresh extraction of the SHA-256-pinned Wine 10.0
archive. The staged source tree is never modified. The PEs remain
unqualified and are not installed in an image (T1 rejects them).

1. `patches/0001-in-process-rpc-only.patch`: remove concrete named-pipe,
   LRPC, TCP and HTTP transports; reject transport lookup/tower parsing and
   return RPC_S_NO_PROTSEQS on enumeration. NDR and in-process code remain.
2. `patches/0002-disable-rpcss-activation.patch`: return server-unavailable
   from RPCSS/ROT/local-server paths before any service/process start or
   retry loop; disable apartment remoting. In-process COM remains upstream.
3. `patches/0003-setupdi-unsupported.patch`: fail the 90 implemented SetupDi
   exports with ERROR_CALL_NOT_IMPLEMENTED and their declared result type's
   failure value. INF/cabinet/registry/file/INI code remains upstream.

4. `patches/0004-single-msvcrt-crt.patch`: add `msvcrt` to the `IMPORTS` of
   every reused module (COMCTL32, COMDLG32, SHELL32, SHLWAPI, OLE32, COMBASE,
   OLEAUT32, RPCRT4, RICHED20/32, CABINET, ADVPACK, SETUPAPI, IMM32, COML2,
   SHCORE), so they link the one system `msvcrt.dll` instead of `ucrtbase.dll`
   (Windows 98 has no UCRT). Keep three UCRT-only calls locally with their
   documented semantics: `_recalloc` (SHLWAPI, SETUPAPI, SHELL32, SHCORE),
   `round()` (SHLWAPI) and `_swprintf_l` (OLEAUT32, mapped to the MSVCRT
   export `_snwprintf_l`, same arguments).
5. `patches/0005-builtin-region-base.patch`: drop RICHED20's upstream fixed
   image base; the recipe links every reused DLL at its CiukiOS builtin-store
   base (packed from `0x74000000`).

Recipe steps that are not source patches: `llvm-objcopy --strip-all` removes
the COFF symbol table that winegcc's mandatory `-debug:dwarf` leaves past
SizeOfImage (no mapped byte changes), and `-Wl,-base:` sets the bases.

Decided but not yet written (recorded per entry in `config/wine-upper.json`):
KERNELBASE retarget to the Windows 98 home DLLs, compile-in of the upstream
kernelbase SHLWAPI units into SHLWAPI, and removal of the three Wine-private
calls in SHELL32 (`wine_get_dos_file_name`, `wine_get_unix_file_name`,
`wine_get_host_version`).

Each patch identifies its pinned base and modification date. Patch and PE
SHA-256 values are recorded in `config/wine-upper.json` (and per run in the
build directory's `build.json`). These are build/subset patches, not proof
that the remaining core ABI matches.
