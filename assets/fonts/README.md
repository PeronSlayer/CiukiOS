# CiukiOS desktop fonts

Ten free fonts, converted to the desktop's bitmap format, plus the built-in
font. They are installed in `\SYSTEM\FONTS`; the Control Panel's Fonts applet
shows them, sets the system font and installs new ones.

| File | Font | Licence | Source |
| --- | --- | --- | --- |
| `CIUKIOS.CFN` | CiukiOS Sans (default) | OFL 1.1 | Liberation Sans, `src/com/setup_font.bin` |
| `NOTOSANS.CFN` | Noto Sans | OFL 1.1 | google/fonts `ofl/notosans` |
| `NOTOSERF.CFN` | Noto Serif | OFL 1.1 | google/fonts `ofl/notoserif` |
| `ROBOTO.CFN` | Roboto | OFL 1.1 | google/fonts `ofl/roboto` |
| `OPENSANS.CFN` | Open Sans | OFL 1.1 | google/fonts `ofl/opensans` |
| `FIRASANS.CFN` | Fira Sans | OFL 1.1 | google/fonts `ofl/firasans` |
| `INTER.CFN` | Inter | OFL 1.1 | google/fonts `ofl/inter` |
| `WORKSANS.CFN` | Work Sans | OFL 1.1 | google/fonts `ofl/worksans` |
| `ATKINSON.CFN` | Atkinson Hyperlegible | OFL 1.1 | google/fonts `ofl/atkinsonhyperlegible` |
| `INCONSOL.CFN` | Inconsolata | OFL 1.1 | google/fonts `ofl/inconsolata` |
| `DEJAVU.CFN` | DejaVu Sans | Bitstream Vera licence, DejaVu changes in the public domain | DejaVu 2.37 release |

- **Sources.** `upstream/` holds the downloaded fonts and their licence
  files. The google/fonts files come from the commit in
  `upstream/GOOGLE_FONTS_COMMIT`. `manifest.json` records the SHA-256 of
  every source and every converted font.
- **Names.** None of the OFL fonts above has a Reserved Font Name, so the
  converted fonts keep their family names. The built-in font comes from
  Liberation Sans, whose name is reserved, so it is called CiukiOS Sans.
- **Licences on the disk.** `LICENSES.TXT` (installed as
  `\SYSTEM\FONTS\LICENSES.TXT`) lists each font's copyright notice and holds
  the full OFL 1.1 and Bitstream Vera/DejaVu licence texts.

## Format (`.CFN`)

A 64-byte header, then the regular and bold styles. Each style has 95
advance widths (characters 32-126) and 95 glyphs of 16 rows; a row is a
little-endian 16-bit word whose bit 15 is the leftmost pixel.

| Offset | Field |
| --- | --- |
| 0 | `CIUKFNT1` |
| 8 | name, 32 bytes |
| 40 | version (1), pixel size, flags (1: bold present), data bytes (6270) |
| 48 | licence id, 16 bytes |

Each font is rasterized in 1-bit, on the built-in font's baseline (row 12),
at the size whose capital H matches its height.

## Building and adding fonts

- `python3 scripts/build_fonts.py` converts every font again (Pillow with
  FreeType).
- `python3 scripts/build_fonts.py --check` verifies the sources and the
  converted fonts; the full build runs it.
- `python3 scripts/build_fonts.py --convert MYFONT.TTF --name "My Font"
  --output MYFONT.CFN` converts any TrueType or OpenType font (`--bold`
  gives its bold face). Copy the `.CFN` to the CiukiOS disk and open it:
  the Fonts applet previews and installs it.
