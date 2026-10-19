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
