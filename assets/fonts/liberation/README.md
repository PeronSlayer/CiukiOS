# Liberation scalable fonts

Liberation Fonts **2.1.5**: twelve unchanged TrueType files, Sans/Serif/Mono
in Regular, Bold, Italic and Bold Italic. They are separate from CiukiOS's
default pre-rasterised desktop fonts; this addition changes no desktop face.

- Upstream: [Liberation Fonts](https://github.com/liberationfonts/liberation-fonts/tree/2.1.5).
- Upstream binary release: [liberation-fonts-ttf-2.1.5.tar.gz](https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-2.1.5.tar.gz).
- License: [LICENSE](LICENSE), **SIL Open Font License 1.1**, including the
  upstream copyright and Reserved Font Name Liberation.
- Credits: [AUTHORS](AUTHORS), retained from the upstream 2.1.5 tag.
- SHA-256 of each TTF and notice: [UPSTREAM.json](UPSTREAM.json). The image
  builder verifies that record before installing fonts and includes each
  installed file's digest in `build-manifest.json`.

Provenance: the twelve TTF files are byte-identical to the upstream release
archive `liberation-fonts-ttf-2.1.5.tar.gz` (SHA-256
`7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0`), extracted and
compared file by file by the lead on 2026-10-13; per-file digests are in
`UPSTREAM.json`. The upstream license and authors were retrieved from the
pinned tag.

Redistribute copyright and OFL text with the fonts. Fonts may be bundled with
software but not sold alone; derivatives must remain under OFL and cannot
retain the Reserved Font Name without permission. This project does not
modify the outlines, metrics or font names. The license does not cover
documents rendered with the fonts or the Ciuki portrait. No Microsoft fonts
are included.

`scripts/build_image.py` installs the twelve faces, `LIBERATION-LICENSE.txt`,
`LIBERATION-AUTHORS.txt` and `LIBERATION-UPSTREAM.json` in `/System/fonts/`.
The Control Panel Fonts page reads family/style from the font's `name` table
and keeps only the selected face open. F5 alias policy lives in
[fonts-contract.md](../../../docs/design/fonts-contract.md).

Since F2-63 (2026-10-14) the rasteriser is FreeType 2.13.3, built by the SDK
from the SHA-256-pinned upstream archive (`config/sdk-pins.json`,
`sdk/freetype/build.py`) with normal grey hinting and TrueType interpreter v35.
Ciuki selects the FreeType License (FTL) and ships `FreeType-FTL.txt`,
`FreeType-LICENSE.txt` and `FreeType-NOTICE.txt` in `/system/licenses/` and the
SDK's `licenses/`. The former stb_truetype 1.26 backend is retired; see
[fonts-contract.md](../../../docs/design/fonts-contract.md).
