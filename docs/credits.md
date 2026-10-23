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
| Linux Synaptics/ALPS protocols | v6.12 | GPL-2.0-only | `docs/licenses/linux-v6.12-touchpad-NOTICES.txt` | image |
| Linux serial mouse wire equations | v6.12 | GPL-2.0-or-later | `docs/licenses/serial-mouse-NOTICE.txt` | image |
| pixman, X11 regions (integrating) | f2-95 | MIT | `third_party/pixman` | integrating |
| Linux ata_piix.c algorithms | v6.12 | GPL-2.0-or-later | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux pata_via.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux pata_sis.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux pata_ali.c algorithms | v6.12 | GPL (v2 selected) | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux pata_amd.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux libata-pata-timings.c algorithms | v6.12 | GPL-2.0-or-later | `docs/licenses/ata-chipset-NOTICES.txt` | image |
| Linux pata_pdc202xx_old.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-addin-NOTICES.txt` | image |
| Linux pata_pdc2027x.c algorithms | v6.12 | GPL-2.0-or-later | `docs/licenses/ata-addin-NOTICES.txt` | image |
| Linux pata_hpt366.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-addin-NOTICES.txt` | image |
| Linux pata_hpt37x.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-addin-NOTICES.txt` | image |
| Linux pata_hpt3x2n.c algorithms | v6.12 | GPL-2.0-only | `docs/licenses/ata-addin-NOTICES.txt` | image |
| dmraid pdc/hpt37x/hpt45x metadata constants | pinned blobs in notice | GPL-2.0-or-later / BSD-3-Clause | `docs/licenses/ata-addin-NOTICES.txt` | image |
| FreeBSD md_promise.c signature | releng/14.2, pinned blob | BSD-2-Clause | `docs/licenses/ata-addin-NOTICES.txt` | image |
| PCI ID Repository | 2026.10.01 | GPL-2.0-or-later | `docs/licenses/hwids-NOTICES.txt` | image |
| USB ID Repository (hwdata) | 2026.06.26 | GPL-2.0-or-later | `docs/licenses/hwids-NOTICES.txt` | image |
| Radeon MMIO 2D register port | Linux 6.12, ATI 6.14.6 | GPL-2.0 and MIT | `docs/licenses/radeon2d-NOTICES.txt` | image |
| X.Org nv 2D engine (integrating) | 0b6619fe | MIT-style | `docs/licenses/nv2d-NOTICES.txt` | integrating |
| NVIDIA PIO FIFO table (integrating) | Linux v6.12 | NVIDIA permissive | `docs/licenses/nv2d-NOTICES.txt` | integrating |
| Intel 2D: Haiku and Intel | r1beta5 / Linux v6.7 | MIT | `docs/licenses/intel2d-NOTICES.txt` | image |
| VIA UniChrome native 2D port | v6.12 / 947c2727 | GPL-2.0 + MIT | `docs/licenses/unichrome-NOTICES.txt` | image |
| SiS 2D (Thomas Winischhofer) | Linux v6.12 / X.Org | GPL-2.0-or-later / BSD-3 | `docs/licenses/sis-2d-NOTICES.txt` | image |
| Matrox native 2D donor (Linux fbdev) | Linux v6.12 | GPL-2.0-only / GPL-2.0 | `docs/licenses/matrox-linux-v6.12-NOTICES.txt` | image |
| VMware SVGA protocol header | 8f0cf7c03597 | MIT | `docs/licenses/vmware-svga-NOTICES.txt` | image |
| Linux Trident Blade/XP engine | v6.12 | GPL-2.0-only | `docs/licenses/trident-blade-NOTICES.txt` | image |
| Haiku ATI Mach64 driver fragments | r1beta5 (a1f00118db8b) | MIT | `docs/licenses/haiku-mach64-NOTICES.txt` | image |
| Haiku ATI 2D driver fragments | r1beta5 | MIT | `docs/licenses/haiku-rage128-NOTICES.txt` | image |
| Linux cirrusfb engine | v6.12 | GPL-2.0 | `docs/licenses/cirrusfb-v6.12-NOTICES.txt` | image |
| Haiku 3dfx driver fragments | r1beta5 | MIT | `docs/licenses/haiku-r1beta5-3dfx-NOTICES.txt` | image |
| Haiku S3 Trio64 and ViRGE fragments | r1beta5 | MIT | `docs/licenses/haiku-s3-r1beta5-NOTICES.txt` | image |
| Linux i810 framebuffer driver | 6.12 | GPL-2.0 | `docs/licenses/intel-legacy-2d-NOTICES.txt` | image |
| X.Org i740 display driver | 1f13c18a1791 | MIT-style | `docs/licenses/intel-legacy-2d-NOTICES.txt` | image |
| NeoMagic 2D engine and cursor | v6.12 / X 6661bdd | GPL-2.0 and X11 | `docs/licenses/neomagic-NOTICES.txt` | image |
| X.Org Chips HiQV engine and cursor | 9187d76bd4f8 | X11/MIT; header unspecified | `docs/licenses/chipshiqv-NOTICES.txt` | image |
| X.Org Riva 128 driver fragments | 0b6619fec39f | MIT | `docs/licenses/riva128-NOTICES.txt` | image |

ACPICA's audited sources are retained under `third_party/acpica/`; the serial
mouse adaptation's exact upstream file and authors are in its notice above.

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
| Yamaha DS-1 microcode | ALSA firmware v1.2.4 | Yamaha, all rights reserved | `third_party/yamaha/NOTICE.txt` | image |
| Yamaha DS-XG driver algorithms | Linux v6.12 | GPL-2.0-or-later | `third_party/yamaha/NOTICE.txt` | image |
| ALSA Trident trident.h | Linux v6.12 | GPL-2.0-or-later | `docs/licenses/f1-120-alsa-NOTICES.txt` | image |
| ALSA Trident trident_main.c | Linux v6.12 | GPL-2.0-or-later | `docs/licenses/f1-120-alsa-NOTICES.txt` | image |
| ALSA ALi ali5451.c | Linux v6.12 | GPL-2.0-or-later | `docs/licenses/f1-120-alsa-NOTICES.txt` | image |
| Linux ALi PCI IRQ readback | Linux v6.12 | GPL-2.0 | `docs/licenses/f1-120-alsa-NOTICES.txt` | image |
| stb_image | 2.30 | MIT or Public Domain | `third_party/stb/LICENSE` | image |
| ALSA EMU10K1/10K2 driver algorithms | Linux v6.12 | GPL-2.0-or-later | `docs/licenses/emu10k1-NOTICES.txt` | image |
| stb_vorbis and dr_libs | pinned | MIT or Unlicense | `third_party/audio/README.md` | image |
| minimp3 | ea99364f | CC0 1.0 | `third_party/minimp3/LICENSE` | image |
| Linux AC'97 driver adaptations | v6.12 | GPL-2.0-or-later / GPL-2.0 | `docs/licenses/linux-ac97-NOTICES.txt` | image |
| TinySoundFont | 0.9 | MIT | `sdk/win32/audio/tsf/LICENSE` | image |
| Linux ESS Maestro PCM algorithms | v6.12 / v2.6.18 | GPL-2.0-or-later | `docs/licenses/maestro-NOTICES.txt` | image |
| Linux C-Media PCM register algorithms | v6.12 | GPL-2.0-or-later | `docs/licenses/linux-cmipci-v6.12-NOTICES.txt` | image |
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

- C-Media origin: Linux v6.12 `sound/pci/cmipci.c`, Copyright (c) 2000 Takashi Iwai
  <tiwai@suse.de>, adapted into `src/kernel/drivers/cmipci.c`; register/rate,
  native mixer, DMA and IRQ algorithms only. PCI constants were checked against
  Linux v6.12 `include/linux/pci_ids.h` (GPL-2.0). Original headers and the
  exact donor blob are recorded in the notice; no ALSA framework is imported.
- The CiukiOS code is GPL-2.0 (`LICENSE`); the SDK and `libciuki` are MIT (`sdk/LICENSE`). Code ported into the unified DOS kernel is GPL-2.0-or-later by the owner's grant.
- The EMU10K1 adaptation retains every imported file's origin, tag, blob identity and original header in `docs/licenses/emu10k1-NOTICES.txt`: Linux v6.12 `sound/pci/emu10k1/{emu10k1_main,emupcm,emufx,io,memory,irq,voice,emumixer}.c`, `include/sound/emu10k1.h` and `include/uapi/sound/emu10k1.h`. The last file retains GPL-2.0+ WITH Linux-syscall-note; no upstream ioctl ABI is imported.

- The Ciuki portrait is a separate project asset, not part of Tango's Public Domain release, and must not be redrawn or replaced. Tango authors and the source archive hash are in `assets/icons/README.md` and `assets/icons/CREDITS.TXT`.
- The Ciuki cursor SVGs and generated rasters are original project artwork drawn by OpenAI Codex for CiukiOS (2026-10-22), GPL-2.0. No Apple artwork is included. The inactive DMZ-White (CC BY-SA 3.0, Novell) and Hackneyed (MIT, Richard Ferreira) notices remain in `assets/cursors/` with their retained material.
- The Wine DLLs are LGPL-2.1-or-later; corresponding source and the written offer are `third_party/wine/SOURCE-OFFER.txt`. A release must carry them before publication resumes.
- Game data are not part of the engines' licences: only public shareware packages are pre-installed, with their own terms (`third_party/games/*/UPSTREAM.json`).
- `src/kernel/drivers/maestro.c` adapts Linux `sound/pci/es1968.c` (v6.12) and `sound/oss/maestro.c` (v2.6.18), by Matze Braun, Takashi Iwai, Alan Cox, Thomas Sailer and Zach Brown; exact origin links and retained notices are in `docs/licenses/maestro-NOTICES.txt`.
- The Microsoft and vendor driver collections under `third_party/drivers` are research references, are not installed and are not redistributed.
- ACPICA sources are retained in `third_party/acpica`; its existing System row, elected GPL-2.0 notice and About entry cover that directory.
- `third_party/yamaha/ds1_microcode.h` imports [ALSA firmware v1.2.4, `ymfpci/hwmcode.c`](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/ymfpci/hwmcode.c), copyright 1997–1999 Yamaha Corporation, All Rights Reserved. Only the array type/const qualification changed; its origin, complete notice and firmware hashes are in `third_party/yamaha/NOTICE.txt` and `UPSTREAM.json`.
- `src/kernel/drivers/ymfpci.c` adapts Jaroslav Kysela's [Linux v6.12 `sound/pci/ymfpci/{ymfpci.c,ymfpci.h,ymfpci_main.c}`](https://github.com/torvalds/linux/tree/v6.12/sound/pci/ymfpci), GPL-2.0-or-later, into Ciuki's PCM transport. The same Yamaha notice file records each donor path and Git blob; the microcode retains its separate notice.
- Reference material that is not shipped: the FreeDOS 1.3 packages of the conformance battery (`third_party/freedos/1.3`).
- pixman and the X11 region algorithms (xorg `miregion.c`, pixman `pixman-region32.c`) are MIT; they enter with the compositor rebuild of directive f2-95 (dev diary 2026-10-22-08), which adds `third_party/pixman` and its notice.

## EMU source origins

EMU donor file credits (all Linux **v6.12**, algorithms/constants adapted into
`src/kernel/drivers/emu10k1.c`; original authors and grants retained in the notice):

| Imported source portion | Origin |
| --- | --- |
| Initialization, board boundary | [sound/pci/emu10k1/emu10k1_main.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emu10k1_main.c) |
| Voice/cache/capture programming | [sound/pci/emu10k1/emupcm.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emupcm.c) |
| DSP instruction generation and routing | [sound/pci/emu10k1/emufx.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emufx.c) |
| Indexed register and codec access | [sound/pci/emu10k1/io.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/io.c) |
| Page table encoding | [sound/pci/emu10k1/memory.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/memory.c) |
| Interrupt causes and acknowledgement | [sound/pci/emu10k1/irq.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/irq.c) |
| Even/odd stereo voice pairing | [sound/pci/emu10k1/voice.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/voice.c) |
| Analog output selection | [sound/pci/emu10k1/emumixer.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/emu10k1/emumixer.c) |
| Engine register definitions | [include/sound/emu10k1.h](https://github.com/torvalds/linux/blob/v6.12/include/sound/emu10k1.h) |
| DSP opcodes, operands and debug definitions | [include/uapi/sound/emu10k1.h](https://github.com/torvalds/linux/blob/v6.12/include/uapi/sound/emu10k1.h) |
