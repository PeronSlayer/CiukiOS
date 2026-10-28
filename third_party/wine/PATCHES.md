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

## F5-20C (2026-10-20)

The decided Wine-side changes of `config/wine-upper.json` are now written;
the recipe applies them after 0005, in this order:

6. `patches/0006-shell32-icon-trim.json` (f5-14B amendment 2): a resource
   filter, not a text patch, because the inputs are binary `.ico` files.
   `scripts/win32/wine_upper.py` (`icon_trim`) keeps, in every
   `dlls/shell32/resources/*.ico`, only the Windows 98 formats (1/4/8 bpp
   images at 16, 32 and 48 px) and drops PNG-compressed and 24/32 bpp images;
   the JSON records the rule and the SHA-256 and image counts of each of the
   43 files before and after (430 images, 1,776,519 bytes in; 235 images,
   357,298 bytes out), and the recipe fails on any difference. SHELL32 drops
   from 9,363,456 to 3,293,184 bytes in memory and from 5,132 to 3,490
   resource directory entries (under the loader's 4,096-entry walk budget,
   so the bound is unchanged).
7. `patches/0007-kernelbase-retarget.patch`: COMCTL32 and SETUPAPI import
   KERNEL32 (default import), SHLWAPI and VERSION instead of KERNELBASE;
   COMCTL32's Str* ordinals 350-375 forward to SHLWAPI.
8. `patches/0008-kernelbase-compile-in.patch`: the upstream
   `dlls/kernelbase/path.c` and `string.c` are compiled into SHLWAPI through
   two wrapper units (built with kernelbase's EXTRADEFS; string.c's copies of
   the KERNEL32/USER32 Char*/lstrcmp*/LoadString functions renamed private),
   with QISearch/GetAcceptLanguagesA/W (`kernelbase/main.c`) and the USKEY
   section of `kernelbase/registry.c` copied unchanged into
   `kernelbase_misc.c`, IsChar*W over GetStringTypeW, the NT-native
   RtlDowncaseUnicodeChar over CharLowerW and IsDBCSLeadByteEx over
   GetCPInfo; `-import` is dropped for the 211 exports; SHLWAPI imports
   KERNEL32 and VERSION instead of KERNELBASE. OLE32 gets
   PathCchFindExtension (unchanged) and drops KERNELBASE.
9. `patches/0009-shell32-wine-private-calls.patch`: SHELL32 no longer calls
   `wine_get_dos_file_name`, `wine_get_unix_file_name` and
   `wine_get_host_version` (no Unix namespace, no host system; the
   recycle bin needs WINEHOMEDIR, never set).
10. `patches/0010-msvcrt-sbcs-code-page.patch` (found by the H1 DllMain run):
    the C locale and `_setmbcp(_MB_CP_SBCS)` use code page 0, as the
    Microsoft CRT documents for a single-byte code page, instead of
    US-ASCII 20127, which the CiukiOS KERNEL32 does not provide; MSVCRT's
    DLL_PROCESS_ATTACH failed on GetCPInfo(20127).

Two consecutive runs give identical PE hashes. MSVCRT has the H0 binding and
H1 DllMain evidence for qualification, and `scripts/build_image.py` stages
qualified reused PEs (`scripts/win32/upper_dlls.py stage_reused`).

## F5-20D (2026-10-20)

No patch changes. The recipe is a cached step of `make build-full` (`make
upper`): the sixteen reused PEs (every qualified entry of
`config/wine-upper.json`) are now distributed in the image, so this patch list
and SOURCE-OFFER.txt travel with them as `/system/licenses/WINE-*`.

Each patch identifies its pinned base and modification date. Patch and PE
SHA-256 values are recorded in `config/wine-upper.json` (and per run in the
build directory's `build.json`). These are build/subset patches, not proof
that the remaining core ABI matches.

## F6-26 (2026-10-28)

11. `patches/0011-directplay-lan.patch`: build DPLAYX/DPWSOCKX against the
    same MSVCRT as the other upper DLLs; TCP/IP host enumeration listener on
    47624, replies, legacy Send, complete TCP writes, compound-address
    selection and an address dialog. Use the existing Win32 event primitives
    for Winsock's event convenience operations. Rearm UDP reception after a
    malformed packet and preserve SP-owned player data on allocation failure.
12. `patches/0012-directplay-lobby.patch`: implement A/W lobby application
    registration, unregistration and enumeration; close enumeration keys;
    bound and quote launch commands; convert Unicode launch connections and
    release process handles. Do not treat a registry write failure as success.
13. `patches/0013-directplay-enumeration.patch`: require a matching, open,
    hosted session and a complete enumeration request; honour availability
    filtering; handle allocation failure and an absent session name.

These extend the pinned sources; they do not establish Windows 98 IPX wire
compatibility or add a DirectPlay 8 runtime. See
[`docs/design/directplay.md`](../../docs/design/directplay.md) for the source
research, test boundaries and outstanding gates. The source offer covers
these patches and the three added PE builds under LGPL-2.1-or-later.

## F6-27 (2026-10-28)

14. `patches/0014-directplay-ipx.patch`: AF_IPX datagrams and SPX streams in
    the actual Wine provider, correctly sized socket addresses and node keys,
    broadcast enumeration and truthful datagram caps. Unsupported migration
    and DirectPlay reliable-protocol session engines fail explicitly. The IPX
    enumeration socket/provider header/SPX selection are documented CiukiOS
    assumptions; Windows 98 wire compatibility is not claimed.
15. `patches/0015-directplay-lobby-lifetime.patch`: transactional A/W launch,
    reservation/settings/notification/resume failure rollback, remote handle
    cleanup, allocation failure preservation, and a notification worker whose
    owned handles and reservation end with the child or launch cancellation.
    The worker retains no lobby-object pointer.

## fx-hlutils: OLE dialogs and WinINet

`0016-wininet-no-tls.patch` applies to Wine 10.0 at the pinned commit. It
links oledlg and wininet to the same reused MSVCRT as the existing upper
DLLs, avoiding an unintended UCRT dependency. Until a Schannel provider is
available, NETCON_secure_connect returns
ERROR_INTERNET_SECURITY_CHANNEL_ERROR before entering security delay imports.
Plain HTTP and FTP retain Wine's ws2_32 calls, resolved to CiukiOS Winsock.
This is an LGPL-2.1-or-later source modification, supplied with the recipe.
The new PEs require the same import closure and H0/H1 evidence as the others.

The same patch makes WinINet's optional legacy MPR password-cache helpers
return ERROR_NOT_SUPPORTED and refuses non-Basic HTTP authentication with
ERROR_INTERNET_LOGIN_FAILURE before SSPI delay imports. Credentials entered
for a request can still use Wine's in-process Basic authentication path.
