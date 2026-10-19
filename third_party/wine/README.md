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
