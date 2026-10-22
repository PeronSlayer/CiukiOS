# Credits and third-party notices

CiukiOS is dedicated to Ciuki, the dog in the original boot splash. *Classic computing, one woof at a time.*

This is the authoritative list of third-party components shipped in the CiukiOS image or used to build it. The system's **About CiukiOS** dialog (Credits tab) shows the same entries (`apps/desktop/credits.inc`); `tests/host/test_credits.py` checks this file against the dialog data, the pins in `config/` and the notice files, so the three cannot drift. Notices installed in the image live in `/system/licenses` (Licences entry of the Credits tab opens that folder in Files).

Status: **image** is installed in the image, **build only** is used to build or test it, **source only** is kept in the repository but not installed, **integrating** is being integrated and its notice is added with the integration.

## System

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| CiukiOS kernel, desktop, tools | this release | GPL-2.0 | `LICENSE` | image |
| CiukiOS SDK and libciuki | this release | MIT | `sdk/LICENSE` | image |
| newlib C library | 4.5.0.20241231 | BSD-style | `sdk/newlib/recipe.json` | image |
| FreeType | 2.13.3 | FTL | `config/sdk-pins.json` | image |
| Liberation, Noto, Inter fonts | see fonts | SIL OFL 1.1 | `assets/fonts/LICENSES.TXT` | image |
| jitterentropy-library | 3.7.0 | BSD-3 or GPL-2.0 | `docs/licenses/jitterentropy-3.7.0-NOTICES.txt` | image |
| ACPICA | 20260930 | GPL-2.0-only (elected) | `docs/licenses/acpica-20260930-NOTICE.txt` | image |
| pixman, X11 regions (integrating) | f2-95 | MIT | `third_party/pixman` | integrating |

## DOS

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| FreeDOS kernel (unified base) | 2043 | GPL-2.0-or-later | `third_party/freedos/1.3/COPYING` | image |
| FreeCOM | 0.85a | GPL-2.0 | `third_party/freedos/1.3/COPYING` | image |
| SHARE | 2006a | GPL | `third_party/freedos/1.3/COPYING` | image |
| CiukiDOS code in the unified kernel | this release | GPL-2.0-or-later | `docs/licenses/CiukiDOS-unified-kernel-GRANT.md` | image |
| Jemm and JLOAD | e96bb6bb | Artistic and MIT | `third_party/jemm/Artistic.txt` | source only |
| CTMouse | fork snapshot | GPL-2.0 | `third_party/ctmouse/copying` | source only |
| DOS Navigator | 1.51 | RIT BSD-style | `third_party/DOSNavigator/README.TXT` | source only |
| VSBHDA and DBOPL (OPL3) | 75fa4bb | GPL-2.0-or-later | `third_party/vsbhda/LICENSE` | image |

## Windows compatibility

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| Wine reused DLLs (16 PEs) | 10.0 | LGPL-2.1-or-later | `third_party/wine/COPYING.LIB` | image |
| musl C library parts in Wine | see notice | MIT | `third_party/wine/musl-COPYRIGHT.txt` | image |
| NSIS installer fixture | 2.51 | zlib/libpng | `third_party/nsis/COPYING` | build only |

## Network and browser

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| lwIP | 2.2.1 | BSD-3-Clause | `docs/licenses/lwip-2.2.1-BSD-3-Clause.txt` | image |
| NetSurf and its libraries | 3.11 | GPL-2.0 with MIT libraries | `config/netsurf.json` | integrating |
| zlib | 1.3.1 | zlib | `config/netsurf.json` | integrating |
| libpng | 1.6.59 | PNG Reference Library v2 | `config/netsurf.json` | integrating |
| BearSSL | 0.6 | MIT | `third_party/bearssl/LICENSE.txt` | image |
| Mozilla CA bundle (via curl) | 2026-09-25 | MPL-2.0 | `third_party/cacert/MPL-2.0.txt` | image |
| MicroQuickJS | 2026-10-04 snapshot | MIT | `third_party/jsvendor/LICENSE` | image |

## Media

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| stb_image | 2.30 | MIT or Public Domain | `third_party/stb/LICENSE` | image |
| stb_vorbis and dr_libs | pinned | MIT or Unlicense | `third_party/audio/README.md` | image |
| minimp3 | ea99364f | CC0 1.0 | `third_party/minimp3/LICENSE` | image |
| TinySoundFont | 0.9 | MIT | `sdk/win32/audio/tsf/LICENSE` | image |
| TJpgDec | R0.03 | ChaN permissive | `third_party/tjpgd/README.md` | image |
| doomgeneric (DOOM engine) | dcb7a8d | GPL-2.0-or-later | `third_party/doomgeneric/LICENSE` | image |
| Wolf4SDL (Wolfenstein 3D engine) | a51c229e | GPL-2.0-or-later | `third_party/wolf4sdl/COPYING` | image |
| DOOM, Quake, Wolf3D shareware | pinned | Shareware terms | `third_party/games/doom-shareware/UPSTREAM.json` | image |

## Tools

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| Tiny C Compiler | 0.9.27 | LGPL-2.1-or-later | `third_party/tcc/COPYING` | image |
| Lua | 5.4.8 | MIT | `sdk/LICENSE` | image |
| Open Watcom C/C++ | 1.9 | Watcom-1.0 | `config/openwatcom.json` | build only |

## Icons

| Component | Version / pin | Licence | Notice | Status |
| --- | --- | --- | --- | --- |
| Tango Icon Theme | 0.8.90 | Public Domain | `assets/icons/README.md` | image |
| Ciuki portrait | approved asset | Project asset, not PD | `assets/brand/ciuki-logo.png` | image |
| Ciuki cursor artwork | f2-99 | GPL-2.0 | `assets/cursors/README.md` | image |

## Notes

- The CiukiOS code is GPL-2.0 (`LICENSE`); the SDK and `libciuki` are MIT (`sdk/LICENSE`). Code ported into the unified DOS kernel is GPL-2.0-or-later by the owner's grant.
- The Ciuki portrait is a separate project asset, not part of Tango's Public Domain release, and must not be redrawn or replaced. Tango authors and the source archive hash are in `assets/icons/README.md` and `assets/icons/CREDITS.TXT`.
- The Ciuki cursor SVGs and generated rasters are original project artwork drawn by OpenAI Codex for CiukiOS (2026-10-22), GPL-2.0. No Apple artwork is included. The inactive DMZ-White (CC BY-SA 3.0, Novell) and Hackneyed (MIT, Richard Ferreira) notices remain in `assets/cursors/` with their retained material.
- The Wine DLLs are LGPL-2.1-or-later; corresponding source and the written offer are `third_party/wine/SOURCE-OFFER.txt`. A release must carry them before publication resumes.
- Game data are not part of the engines' licences: only public shareware packages are pre-installed, with their own terms (`third_party/games/*/UPSTREAM.json`).
- The Microsoft and vendor driver collections under `third_party/drivers` are research references, are not installed and are not redistributed.
- Reference material that is not shipped: the FreeDOS 1.3 packages of the conformance battery (`third_party/freedos/1.3`).
- pixman and the X11 region algorithms (xorg `miregion.c`, pixman `pixman-region32.c`) are MIT; they enter with the compositor rebuild of directive f2-95 (dev diary 2026-10-22-08), which adds `third_party/pixman` and its notice.
