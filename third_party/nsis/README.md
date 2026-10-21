# NSIS 2.51 authored test fixture

The source and Windows release ZIP are pinned by the lead in
`config/nsis.json`. `scripts/build_nsis.py` verifies both archives before
building the host compiler. There is no download or system-NSIS fallback.
Upstream: <https://nsis.sourceforge.io/>; release:
<https://sourceforge.net/projects/nsis/files/NSIS%202/2.51/>.

CiukiOS authors the installer script and payload. The compiled installer
embeds the **unchanged release zlib stub** and uses zlib compression. It
embeds no third-party plugins. `COPYING` is the unmodified notice from the
source archive; the fixture inventory also carries it as NSIS-COPYING.TXT.
NSIS is copyright 1995–2015 its contributors; the installer/stub and zlib
compressor use the zlib/libpng license. The host compiler additionally
contains upstream bzip2 and CPL-1.0 LZMA compressors; their notices are also
in COPYING. This compiler is a host build tool and is not staged in the OS.

The host sources are **modified** for native pointer width: preserve 32-bit
Windows LONGs and export-name RVAs, widen host-only resource-editor pointer
bookkeeping and pointer casts, and include missing POSIX/integer declarations.
The exact transformations are maintained in `scripts/build_nsis.py` and
recorded by the build in `build/tools/nsis/build.json`, alongside compiler
identity, hashes and upstream configuration defaults. The Windows stub,
compression modules and VM bytecode engine are not altered.
