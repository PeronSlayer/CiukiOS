# Wine provenance for CiukiOS

Wine 10.0, tag `wine-10.0`, commit
`b073859675060c9211fcbccfd90e4e87520dc2c2`.
Upstream: https://gitlab.winehq.org/wine/wine/-/tree/wine-10.0
Mirror: https://github.com/wine-mirror/wine/tree/b073859675060c9211fcbccfd90e4e87520dc2c2

The P5 DLLs are bounded CiukiOS implementations, LGPL-2.1-or-later, rather
than linked Wine PE binaries. Algorithm references and tables were checked
against that exact commit: ADPCM (Eric Pouech), MS Video 1 (FFmpeg project,
Mike Melanson, Mike McCormack), Cinepak (Dr. Tim Ferguson, Mike McCormack),
MS RLE (Michael Günnewig). See `docs/design/win32-wine-dependency-audit.md`.
`codecs/` retains only the unmodified export specifications and Makefiles
used as interface metadata. These are not a staged Wine source tree.
Their upstream content hashes are in `codecs-sources.json`.

`LICENSE`, `COPYING.LIB` and `AUTHORS` are the pinned upstream notices.
Original CiukiOS codec sources live in `sdk/win32/codecs/`; their build and
coverage inventory is produced by `scripts/win32/codecs.py`. No Wine source
patch is applied by this P5 recipe; `PATCHES.md` records that fact. This
statement does not claim that a future P4 recipe has no patches.

The release archive is still pending-download in `config/wine-pin.json`.
Do not claim archive verification from individually fetched tagged files.

F5-14B currently supplies original CiukiOS `dsetup.dll` and `dsetup32.dll`
from `sdk/win32/upper/dsetup.c`; these are LGPL-2.1-or-later and contain no
Wine implementation. Rebuild them with
`scripts/win32/build-wine-dlls.sh --installer-builtins`. The existing IMM32
is still a census-sized W21 placeholder, not a reused Wine IME.

No P4 Wine PE is qualified or shipped by this delivery. Its full source cache
is absent and the archive download failed (DNS). `--upper-audit` checks
actual PE imports/forwarders; `--upper-source-audit` compares source-reference
candidates with the built core. Neither command converts an unbuilt DLL into
a qualified binary. T1 rejects a reused-PE manifest entry until a subsequent
delivery implements complete source/patch/build/load qualification. A complete
corresponding-source distribution must accompany any future reused P4 DLLs;
the upstream URLs above alone do not meet that requirement.

### F5-14B continuation

The full release archive is now locally SHA-256 verified (`archive-verified`
in `config/wine-pin.json`), superseding the pending-download statements above.
`scripts/win32/build-wine-dlls.sh --upper-build` builds seventeen upper DLLs
from that archive (sixteen to be reused, IMM32 measured only), using upstream
configure/makedep/IDL/resource rules and clang/lld. It applies the five
patches listed in PATCHES.md to a private extraction. It does not build
Wine's core DLLs, server or Unix runtime. `config/wine-upper.json` records
the exact PE hashes (identical on two consecutive runs), bases, sizes,
section-store page models, the licence entry and one decision per missing
core export. The production builtin store continues to reject reused Wine
records (unlisted, mismatched or unqualified). No binary is distributed in an
OS image by this continuation.

### F5-20C

The recipe applies ten recorded changes (PATCHES.md: the five above, the
SHELL32 icon-format filter 0006, the KERNELBASE retarget and compile-in
0007/0008, the SHELL32 Wine-private call removal 0009 and the MSVCRT C-locale
code page 0010). `scripts/build_image.py` stages qualified reused PEs in the
image's builtin store, with this directory's LICENSE, COPYING.LIB, AUTHORS,
SOURCE-OFFER.txt, PATCHES.md and README.md as `/system/licenses/WINE-*`.

### F5-20D: the reused PEs ship in the image

`make build-full` builds the reused DLLs as a cached step (`make upper`,
`scripts/win32/wine_upper.py cached-build`, output `build/win32/upper`), so the
canonical image now **distributes sixteen Wine 10.0 PEs under
LGPL-2.1-or-later**: advpack, cabinet, combase, comctl32, comdlg32, coml2,
msvcrt, ole32, oleaut32, riched20, riched32, rpcrt4, setupapi, shcore, shell32
and shlwapi (`config/wine-upper.json`, `dlls.*.qualified`; IMM32 is built and
measured but not reused and never staged). Each is qualified by its recorded
H0 binding and H1 DllMain evidence on that exact SHA-256, and the guest case
`w32-upper-dlls-qemu-t23` is the gate for their use.

Patches applied to the unmodified Wine 10.0 archive, in this order (details in
PATCHES.md, hashes in `config/wine-upper.json`):

1. 0001 in-process RPC only; 2. 0002 no RPCSS activation; 3. 0003 SetupDi
unsupported; 4. 0004 single MSVCRT; 5. 0005 builtin-region image bases;
6. 0006 SHELL32 icon-format filter (Windows 98 formats only); 7. 0007
KERNELBASE import retarget; 8. 0008 KERNELBASE path/string code compiled into
SHLWAPI and OLE32; 9. 0009 SHELL32 Wine-private calls removed; 10. 0010 MSVCRT
single-byte C locale.

F6-26 adds the same pinned Wine's `dplayx`, `dplay` and `dpwsockx` builds,
with patches 0011 (TCP/IP LAN provider), 0012 (lobby applications) and 0013
(session enumeration). These are LGPL-2.1-or-later source modifications;
the exact patches and hashes accompany the existing source offer. IPX and
end-to-end title qualification are tracked in `docs/design/directplay.md`.

Obligations that come with distributing these PEs: the LGPL-2.1 text
(COPYING.LIB), Wine's LICENSE and AUTHORS, this README, PATCHES.md and
SOURCE-OFFER.txt are in the image as `/system/licenses/WINE-*`; the
bundled musl and zlib notices (`musl-COPYRIGHT.txt`, `zlib-NOTICE.txt`, copied
from the hash-pinned Wine archive) accompany the reused MSVCRT and CABINET
under their upstream terms. The
corresponding source (the exact archive named by `config/wine-pin.json`, the
patches in `patches/`, the complete recipe and helpers in `scripts/win32/`,
the pin and `config/wine-upper.json`) must accompany any published copy of the
image, as described in SOURCE-OFFER.txt. The users may replace any of the PEs
by rebuilding them with that source (`make upper`).
