# Video driver families: census and port contract

Research date: **2026-10-22** (workstation `date` checked before writing).
Directive: [f2-102](../directives/f2-102-video-driver-families-census.md).
Status: **proposed for lead review; research only**. No production driver,
new capability, hardware qualification or image result is delivered here.
The complete directive in this checkout has 41 lines and no separate amendment
or lead-note section. This contract targets [DM1–DM13](display-driver-model.md)
and the [1998–2004 hardware baseline](hardware-baseline.md).

**Recommendation:** implement small native ports per chip family behind the
common display registry and software engine. Use Haiku, fbdev and X.Org as
source donors, not as three operating-system compatibility subsystems.
VBE remains the initial mode provider. Acceleration on every retro PC cannot
be promised from a source census: variants, laptop panels, firmware and silicon
errata still need physical qualification. The common software path is what
makes the image usable when a native engine cannot be enabled.

## 1. Evidence and integration boundary

The Windows 98 Resource Kit, chapter 28, “Graphics Subsystem”, describes the
DIB engine and universal driver/minidriver split; chapter 26, “Setting Graphics
Compatibility Options”, describes disabling cursor, blits/MMIO and finally all
acceleration. Inspected original text: `build/rskt/wrkc28_8.htm:73–84` and
`wrkc26_5.htm:12–27` in the owner's read-only main checkout. These support DM4's
software fallback and independent capability refusal, not execution of Windows
binary display drivers. No proprietary text or binaries are imported.

Repository comparison, limited to the relevant interfaces:

- `src/kernel/include/ciuki/accel.h:10–40` provides synchronous fill, copy,
  pattern, write, mono cursor, off-screen rows, pause and write verification.
- `src/kernel/drivers/fbdev.c:10–19,213–275` still names the existing Savage
  implementation and its attempt/fallback hooks. DM1–DM13 are a prerequisite,
  not an already implemented universal ABI in this worktree.
- `src/kernel/drivers/savage2d.c:143–293,596–637` provides the existing bounded
  failure policy, PCI matching and engine construction. Preserve its evidence;
  do not replace it wholesale with a less defensive upstream loop.
- [F7 contract](f7-3d-acceleration-contract.md), §§3.2, 8–9, still defines a
  native triangle service, shared 2D/3D owner, software reference and T23 gates.
  Section 7 below identifies the separate Mesa/OpenGL integration decision.

All upstream snapshots are pinned below. GitHub's source connector supplied
UTF-8 source into ignored `build/research/f2-102/`; shell DNS/network access
was unavailable. No source archive is placed elsewhere. Mirrors are identified
explicitly; neither an indexed snippet nor a project-wide licence label is
used as permission to copy an unaudited file.

## 2. Display-class census

Read **909** case-insensitive `.inf` files in the owner's read-only collection.
Select `[Version] Class=Display` or its setup class GUID
`{4d36e968-e325-11ce-bfc1-08002be10318}`; all **131** selected files have both.
Resolve neutral/localized `[Strings]`, parse the `[Manufacturer]` model sections
and their decorated forms, strip comments and line continuations, and count
unique `VEN:DEV` pairs in model rows. Subsystem IDs, language copies, INF
versions and repeated model rows do not multiply a device. Exclude control
flags, registry strings, AGP bridge names and DisplayCodec. This produces
**404 pairs**, of which **202** are in `assets/hwids/pci-ids.json` and **202**
are missing. The JSON currently has 1,164 pairs (the README's older merged
count is historical); its SHA-256 is
`8af2cacb42ae8f71be3d9ea01eac153aaddad2e2af0393b92e18179482f366fc`.

The extractor in `scripts/build_hwids.py:75–97` requires a `%description%`
token on the left side; literal ATI/S3 descriptions explain the missing rows.
**Do not infer absence from this JSON**, and do not change the generator or
identity assets in this research directive. Its separate fix needs a directive.

| Family represented in Display model rows | Unique pairs | Present in JSON | Interpretation |
| --- | ---: | ---: | --- |
| ATI Radeon, all generations in the pack | 197 | 0 | R100/R200/R300/R400 dominate the in-scope ATI candidates, but the pack also contains R500/Xpress and later refreshes. No Mach64 or Rage128 models. |
| NVIDIA TNT through GeForce6 lineage | 140 | 140 | TNT/TNT2 6; GeForce2 7; GeForce3 4; GeForce4 32; FX 40; GeForce6 51. No Riva128 or GeForce256 model. Includes Quadro/mobile/rebadged and later same-lineage products. |
| NVIDIA GeForce7/later | 27 | 27 | Excluded from the requested engine families; not retro-baseline demand. |
| Intel i830/i845/i855/i865/i915G | 6 | 6 | `3577 2562 3582 2572 2582 2782`; the last is the i915G secondary function, not another board. |
| Intel later/mobile915GM and newer | 14 | 14 | Kept separate from the explicitly requested 1998–2004 set. |
| VIA UniChrome proper (`1106`) | 4 | 2 | `3108 3118 3122 7205`; labels include UniChrome Pro. |
| VIA/Trident shared integrated lineage (`1023`) | 2 | 2 | `8400 8500`; count once, not once per vendor branding. |
| Matrox G200/G400/G450/G550 | 4 | 4 | `0520 0521 0525 2527`; G400 and G450 share an ID. |
| SiS 315/330 and integrated derivatives | 5 | 5 | `0325 0330 6325 6330 6342`; no SiS300 model; later derivatives do not establish 2004 coverage. |
| Trident CyberBlade XP | 1 | 0 | `1023:8820`. |
| S3 SuperSavage | 1 | 0 | `5333:8c2e`, including an IBM subsystem model. |
| VMware SVGA | 2 | 2 | `15ad:0405 15ad:0710`. |
| IBM FireGL2/3/4 outside requested ATI lineage | 1 | 0 | `1014:0170`; not a Radeon merely because an INF says ATI FireGL. |
| **Total** | **404** | **202** | Counts measure this collection's coverage, not installed market share. |

Zero Display models were found for Mach64/Rage Mobility, Rage128, Riva128,
GeForce256, Intel i740/i810/i815, 3dfx, S3 Trio/ViRGE/ProSavage, Cirrus,
NeoMagic, Chips & Technologies, QEMU std/bochs or VirtualBox-specific IDs.
The E500 remains a required target despite that zero. The source collection
contains much newer hardware and many OEM/secondary-function aliases: it is
not a statistical sample of PCs sold in 1998–2004.

Reproducibility fingerprint: SHA-256 over sorted Display INF relative paths,
NUL, original file bytes, NUL is
`896430a321316c37eb42d86608255e6294e6698ba746a867a21a17596b23aa9b`.
No INF paths, vendor prose or private payloads enter the tracked document.
A session-only census program and aggregate IDs remain under
`build/research/f2-102/`; there is no census script in the build or image.

### ATI generation detail used for ordering

Classify primary functions using Linux v6.12 `include/drm/drm_pciids.h`.
Associate explicitly labelled secondary functions by their **model description**
and an explicit list, not by a generic arithmetic offset. Such associations
are census labels, never binding rules. The aggregate groups are R100 lineage
**11**, R200 lineage **23**, R300/R400 lineage **119**, other ATI **44**.
The broad R300/R400 group includes X700/X800/X850, mobility and later refreshes;
it is deliberately not a claim that all 119 products existed in 2004.
The 44 excluded entries are Xpress/R500-class; no entire ATI vendor match is
permitted. The list of primary and secondary IDs must be filtered against the
chosen donor's exact match table in each implementation directive.

## 3. Snapshot catalogue and notation

| Key | Exact source revision and paths |
| --- | --- |
| H | [Haiku r1beta5](https://github.com/haiku/haiku/tree/r1beta5), commit `a1f00118db8b858feb73f64ac4dc3d038b82bf9b`. Accelerants: `src/add-ons/accelerants/<family>/`; paired kernels: `src/add-ons/kernel/drivers/graphics/<family>/`; private headers: `headers/private/graphics/`. Both halves are surveyed, including their different notices. |
| L | [Linux v6.12 fbdev](https://github.com/torvalds/linux/tree/v6.12/drivers/video/fbdev), `drivers/video/fbdev/` plus `include/video/`. Use this stable tag for donor work. |
| LI | [Linux v6.7 intelfb](https://github.com/torvalds/linux/tree/v6.7/drivers/video/fbdev/intelfb), `intelfbdrv.c` and `intelfbhw.c`. Last mainline release series with intelfb; removed for v6.8 in [689237ab37c59b9909bc9371d7fece3081683fba](https://github.com/torvalds/linux/commit/689237ab37c59b9909bc9371d7fece3081683fba). Its dependence on global firmware `screen_info` is a stated removal reason. |
| LA | [Linux fbdev snapshot check](https://github.com/torvalds/linux/tree/a1c2b1da93f5f612aa7a33190c4900c98e51f6da/drivers/video/fbdev), commit `a1c2b1da93f5f612aa7a33190c4900c98e51f6da`. All other named fbdev families remain in this inspected tree; do not confuse removal of their legacy DRM counterparts with removal of fbdev. This is a pinned availability observation, not a maintenance promise. |
| XA | [X.Org ATI 6.14.6 UMS](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/tree/xf86-video-ati-6.14.6/src), tag `xf86-video-ati-6.14.6`. Modern KMS-only DDX files are not a substitute for the older native mode/2D path. |
| XI | [X.Org Intel 2.9.1](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/tree/2.9.1/src), tag **`2.9.1`** (without an `xf86-video-intel-` prefix). Historical UMS/UXA i830 references, separate from the pinned later intel tree. |
| M21 | [Mesa 21.3.9](https://github.com/chaotic-cx/mesa-mirror/tree/mesa-21.3.9), classic Amber-era drivers at `src/mesa/drivers/dri/{radeon,r200,i915,nouveau}` and Gallium at `src/gallium/drivers/{r300,nouveau/nv30,i915,crocus}`. This is a readable Git mirror of Mesa; original project: [Mesa](https://gitlab.freedesktop.org/mesa/mesa). |
| M7 | [Mesa 7.11](https://github.com/chaotic-cx/mesa-mirror/tree/mesa-7.11/src/mesa/drivers/dri), tag `mesa-7.11`; DRI1 `mga,r128,savage,tdfx,sis,unichrome,i810`. |
| D62 | [Linux v6.2 legacy DRM](https://github.com/torvalds/linux/tree/v6.2/drivers/gpu/drm), `mga,r128,savage,tdfx,sis,i810`, and `via/via_dri1.c`. These are F7 service references, not a new fbdev requirement. |
| Q | [QEMU v9.2.0 display models](https://github.com/qemu/qemu/tree/v9.2.0/hw/display): `vga-pci.c`, `bochs-display.c`, `cirrus_vga.c`, `ati.c`, `ati_2d.c`, `vmware_vga.c`. These describe proposed tests; no QEMU was run. |
| V | [VirtualBox source snapshot](https://github.com/mirror/vbox/blob/74117a1cb257c00e2a92cf522e8e930bd1c4d64b/src/VBox/Devices/Graphics/DevVGA-SVGA.cpp), commit `74117a1cb257c00e2a92cf522e8e930bd1c4d64b`. Emulator reference only; GPL-3.0-only code is excluded from Ciuki kernel reuse. |

X.Org source pins below are upstream Git history preserved by the
`freedesktop-unofficial-mirror` repositories, not new driver forks. Canonical
project repositories are under `https://gitlab.freedesktop.org/xorg/driver/`.
Every X-family entry uses `src/` beneath the indicated revision. Section 9
links the exact individual files and records their notices; the mirror name
is part of provenance, not an assertion that it is the official hosting site.

| [X](#3-snapshot-catalogue-and-notation) family | Exact commit in readable mirror / canonical project |
| --- | --- |
| nv | [`0b6619fec39fada995646fb95bb25e72d3c9a491`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/tree/0b6619fec39fada995646fb95bb25e72d3c9a491/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-nv) |
| ati | [`43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/tree/43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-ati) |
| r128 | [`d05022a1c398787d3f21d023a8251cb128ba9f9f`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-r128/tree/d05022a1c398787d3f21d023a8251cb128ba9f9f/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-r128) |
| mach64 | [`810572536e153ac9e4615a35e2ab99dc266806da`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mach64/tree/810572536e153ac9e4615a35e2ab99dc266806da/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-mach64) |
| mga | [`8ed77eb7920f375e9490a31f907eccb02fb123cd`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mga/tree/8ed77eb7920f375e9490a31f907eccb02fb123cd/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-mga) |
| intel | [`83b8528773a0b1e6655fa87c9645a0062cee1359`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/tree/83b8528773a0b1e6655fa87c9645a0062cee1359/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-intel) |
| i740 | [`1f13c18a17914a6e919e604d1dc5589eb18f0141`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/tree/1f13c18a17914a6e919e604d1dc5589eb18f0141/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-i740) |
| tdfx | [`278042ff4c469f24ab7bc28e517e8a78e2821ddc`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/tree/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-tdfx) |
| s3 | [`ed0fbfd27609e897c42d7ab316481a9504746233`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3/tree/ed0fbfd27609e897c42d7ab316481a9504746233/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-s3) |
| s3virge | [`211e2831dcf13c6e5847f6caf894f559d9104c3d`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/tree/211e2831dcf13c6e5847f6caf894f559d9104c3d/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-s3virge) |
| savage | [`d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/tree/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-savage) |
| sis | [`0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-sis/tree/0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-sis) |
| trident | [`64c02f307d7146641adf65e31bcaa2d1238479e7`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/tree/64c02f307d7146641adf65e31bcaa2d1238479e7/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-trident) |
| neomagic | [`6661bdd4551e4e63e983685464a277845aed3012`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-neomagic/tree/6661bdd4551e4e63e983685464a277845aed3012/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-neomagic) |
| cirrus | [`df389885adf71ed3b045c2fde9fd3ba4329e1a58`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-cirrus/tree/df389885adf71ed3b045c2fde9fd3ba4329e1a58/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-cirrus) |
| chips | [`9187d76bd4f8d500fc3403b6d4a7bef55508bf2b`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/tree/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-chips) |
| vmware | [`8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-vmware/tree/8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89/src); [upstream](https://gitlab.freedesktop.org/xorg/driver/xf86-video-vmware) |
| openchrome | NetBSD upstream distribution mirror, commit [`947c2727683984b334ebe249fd981dad3e7a2f44`](https://github.com/NetBSD/xsrc/tree/947c2727683984b334ebe249fd981dad3e7a2f44/external/mit/xf86-video-openchrome/dist/src); [upstream](https://gitlab.freedesktop.org/openchrome/xf86-video-openchrome). Source files specify their own MIT-style terms. |

Capability notation describes **implemented donor paths**, not enabled Ciuki
features: **F** fill, **C** overlap-safe copy, **Y** sync, **M** mono expansion,
**W** native-colour host write, **I** invert (including all-ones XOR), **K**
source key, **S** scaled copy, **L** solid line, **P** mono pattern, **U** mono
cursor, **A** ARGB cursor. `*` means generation/format/compile-option limited
or only an internal primitive; it needs per-call gating. An omitted command
has **no verified exposed hardware path in the inspected donor**, even if the
silicon might support it. It must use DM4, not an invented capability bit.
CPU writes after sync and `cfb_*` are software, not M/W engine acceleration.
Overlay/video scaling is separate from DM2 S; an XVideo or Haiku overlay
hook does not imply general RGB stretch-copy support. All native backends
must supply F/C/Y to bind as accelerated; pure mode drivers cannot.

Sizes below are **engineering estimates in thousands of source lines (kLOC)**,
including comments, for the initial native adapter plus host tests and bounded
poll/error handling, excluding the common shim and later native mode/overlay
work. They are not measured final binaries. H/L/X alternative port estimates
share this definition; retaining a whole upstream OS driver would be larger.
H = Haiku APIs; L = Linux fbdev APIs; X = X.Org DDX/XAA/EXA APIs, as detailed
in section 5. Native mode code in a donor is not automatically imported.

## 4. Families and proposed donor selections

PCI IDs below are explicit **examples plus authoritative source tables**, not
vendor-wide registry masks or exhaustive qualification lists. Each follow-up
must expand the pinned table into an explicit whitelist, check PCI display
class, revision, BARs, aperture ownership and active VBE framebuffer binding.
Secondary functions are excluded unless proven to own the active scanout.
The listed hardware test applies to every donor for that family.

### ATI Mach64 / Rage Pro / Rage Mobility — choose H

Required: E500 ATI Rage Mobility (read the actual unit's ID before binding).
Candidate vendor `1002`: Rage Pro `4742 4744 4749 4750 4751`, Mobility
`4c4d 4c4e 4c52 4c53`; older Mach64 `4758 4358` are a **separate extension**:
Haiku's ATI kernel table is narrower than atyfb's. Use H `ati/driver.cpp`
chip table and L `atyfb_base.c` device table, not a “Mach64 compatible” mask.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[H](#3-snapshot-catalogue-and-notation) `ati/mach64_draw.cpp`, `mach64_init.cpp`, `mach64_util.cpp`, `mach64.h`, `mach64_cursor.cpp`; paired kernel `ati/driver.cpp`** | MIT; generic `engine.cpp`/`cursor.cpp` are Be Sample BSD-style, separately audited | F C Y I U | Native Mach64 mode code; video scaler exists in `mach64_overlay.cpp` | **1.8–3.2 kLOC**, H; compact PIO/MMIO path and E500 relevance. Preserve firmware/panel state; no wholesale clock reset. | `1002:4742/4c4d`; E500 hardware |
| [L](#3-snapshot-catalogue-and-notation) `aty/atyfb_base.c`, `mach64_accel.c`, `atyfb.h`; `include/video/mach64.h` | GPL-2.0; register header GPL-2.0-or-later, compatible | F C Y M | Native; no verified overlay API | 2–4 kLOC, L; broad chip/PLL/platform branches. | `1002:4742/4c4d`; E500 hardware |
| [X](#3-snapshot-catalogue-and-notation) `mach64/atimach64accel.c`, `atimach64cursor.c` | permissive X11 notice, compatible | F C Y M W I K* L P U | Native UMS; XVideo scaler in separate file | 2.5–4.5 kLOC, X; pitch/24bpp and transfer padding. | `1002:4742/4c4d`; E500 hardware |

Test: **E500 hardware only**. QEMU `ati-vga` is not Mach64. Initial H F/C/Y/I/U,
with all other commands punted; native modes and video overlay remain later
provider work. A zero pack count does not justify dropping this laptop.

### ATI Rage 128 / Rage 128 Pro — choose H

Vendor `1002`: examples `5245 5246 524b 524c`, Pro `5046`; Mobility M3
`4c45 4c46`. H `ati/driver.cpp` and L `aty/aty128fb.c` contain separate lists.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[H](#3-snapshot-catalogue-and-notation) `ati/rage128_draw.cpp`, `rage128_init.cpp`, `rage128.h`, `rage128_cursor.cpp`; kernel `ati/driver.cpp`** | MIT | F C Y I U | Native; video scaler in `rage128_overlay.cpp` | **1.6–3 kLOC**, H; use with VBE first. FIFO/cache/reset sequencing needs bounded errors. | `1002:5245/5046`; QEMU Pro subset + hardware |
| [L](#3-snapshot-catalogue-and-notation) `aty/aty128fb.c` | GPL-2.0-only | Y; F/C/M/W use generic software helpers | Native; no exposed overlay | 2–4 kLOC mode port; **not an accelerated F/C donor** at v6.12 (`aty128fb_ops:505–515`). | `1002:5245/5046`; QEMU Pro subset + hardware |
| [X](#3-snapshot-catalogue-and-notation) `r128/r128_accel.c`, `r128_cursor.c` | MIT-style, compatible | F C Y M W I K* L P U | Native UMS; XVideo overlay | 2.5–4.5 kLOC, X; CPU transfer alignment and engine reset. | `1002:5245/5046`; QEMU Pro subset + hardware |

Test: lead may use **`-device ati-vga,model=rage128p`** for the QEMU subset;
physical Rage128/M3/Pro variants remain hardware only. No pack models.

### ATI Radeon R100 / R200 / R300 through X800 (R420/R423) — choose L core + XA optional operations

One `radeon2d` native family, with explicit generation subgroups; X800 is
**R4xx**, not literally R300. Examples vendor `1002`: R100 `5144`, RV100
`5159`, Mobility M6 `4c59`; R200 `514c`, RV250 `4966`, RV280 `5961`;
R300 `4e44`, RV350 `4150`; R420 `4a49`, R423 `5549`. Use L
`aty/radeon_base.c:98–224`, `include/drm/drm_pciids.h`, and XA chipset tables.
Do not enable R500, Xpress variants or secondary functions by vendor match.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `aty/radeon_accel.c`, `radeonfb.h`; register definitions `include/video/radeon.h`** | GPL-2.0; `radeon_base.c` has GPL module plus retained MIT portions | F C Y; imageblit is synchronized CPU software | Native mode donor; no fbdev overlay | **2–3.5 kLOC**, L; common MMIO F/C over a broad device table, no initial CP firmware/GART dependency. | `1002:5144/4c59/514c/4e44/4a49/5549`; QEMU RV100 subset + hardware |
| **[XA](#3-snapshot-catalogue-and-notation) `radeon_accel.c`, `radeon_accelfuncs.c`, `radeon_cursor.c`, `radeon_reg.h`** | individually MIT-style | F C Y M W I K* L P U A* | Native UMS; overlay/textured video separate | **additional 2–4 kLOC**, X; optional operations only after F/C validation. Keep MMIO branch; CP/DMA is separate scope. | `1002:5144/4c59/514c/4e44/4a49/5549`; QEMU RV100 subset + hardware |
| [H](#3-snapshot-catalogue-and-notation) `radeon/Acceleration.c`, `CP.c`, `Cursor.c`, kernel `radeon/driver.c` | copyright-only headers in these files: **hold**, not MIT by inference | F C Y I; cursor hooks commented out in `GetAccelerantHook.c:76–78` | Native mode and overlay code; unaudited coverage for each generation | 3–6 kLOC, H; CP/GART/shared memory complexity and licence gaps. Not selected. | `1002:5144/4c59/514c/4e44/4a49/5549`; QEMU RV100 subset + hardware |

QEMU: **RV100 `1002:5159` only**, alongside the Rage128 model above. QEMU
v9.2.0 `ati.c:44–46,974–980` accepts only `rage128p`/`rv100` (`5046`/`5159`).
**There is no M6 `4c59` model in this snapshot.** The directive's “Radeon M6”
wording must not become a false test claim. R200/R300/X800 and the laptop M6
require hardware. QEMU `ati_2d.c:93,188–202` implements a limited ROP subset;
it cannot validate all XA operations or 3D. Initial mode setting stays VBE.

### NVIDIA Riva 128 — choose X Riva

PCI `12d2:0018` (NV3); do not
confuse TNT NV4 with Riva128 NV3. Final whitelist follows X `riva_driver.c`
and L `riva/fbdev.c`; verify which IDs each donor actually accepts.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[X](#3-snapshot-catalogue-and-notation) nv `riva_xaa.c`, `riva_cursor.c`, `riva_hw.h`** | MIT-style | F C Y M I L P U | Native UMS; no verified scaler path in this subset | **2–3.5 kLOC**, X; distinct object setup and cursor, not NV4 commands. | `12d2:0018`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `riva/fbdev.c`, `riva_hw.c` | GPL wrapper; NVIDIA legacy permissive notice plus explicit GPL-compatibility explanation in hardware file | F C Y M I U | Native; no fbdev overlay | 2–4 kLOC, L; prefer explicit MIT X source over the older notice form. | `12d2:0018`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `nvidia` | Be Sample wrappers; engine files lack an explicit grant: **hold** | No verified NV3 path; do not extrapolate NV4+ hooks | Not established for NV3 | No port estimate until matching/coverage exists. | `12d2:0018`; hardware only |

Test: **hardware only**. No pack models. Later Mesa nouveau backends in
section 7 start at TNT/NV4, not Riva128.

### NVIDIA TNT/TNT2, GeForce256/2/3/4, FX and GeForce6 — choose X nv

Vendor `10de`: TNT `0020`, TNT2 `0028`; GeForce256 `0100`, GeForce2
`0110 0150`, GeForce3 `0200`, GeForce4 `0171 0250`, FX `0301 0322`,
GeForce6 `0040 0140`. The explicit X `nv_driver.c` chipset table is the
matching authority; no `10de:*` wildcard. Families include several distinct
NV4/NV10/NV20/NV30/NV40 engines despite shared command abstractions.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[X](#3-snapshot-catalogue-and-notation) nv `nv_xaa.c`, `nv_dma.h`, `nv_local.h`, `nv_hw.c`, `nv_cursor.c`** | MIT-style on each; `nv_type.h` lacks a header and is excluded pending provenance | F C Y M W I L P U A* | Native UMS; XVideo scaler in `nv_video.c`, not DM2 S | **3–5 kLOC** pre-NV40; **+2–3 kLOC** NV40/GeForce6, X; VRAM pushbuffer wrap/prefetch and generation-specific objects. Recreate native state types, not an X server. | `10de:0020/0100/0110/0200/0250/0322/0040`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `riva` (older) / `nvidia` (NV4+) | GPL wrappers; NVIDIA permissive hardware/engine files, with GPL compatibility note | F C Y M I U | Native; no fbdev overlay | 3–6 kLOC, L; cannot assume cursor or all bpp on every generation. | `10de:0020/0100/0110/0200/0250/0322/0040`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `nvidia/GetAccelerantHook.c`, `engine/nv_acc.c`, `nv_acc_dma.c`, kernel `nvidia/driver.c` | Be Sample wrapper compatible; engine headers do not specify a licence: **hold** | F C Y I S* U; scaled hook advertised, generation-gated | Native; back-end scaler hooks | 3–6 kLOC, H; PIO explicitly fails on NV40+ (`nv_acc.c:10–33`), so it is not a universal PIO solution. | `10de:0020/0100/0110/0200/0250/0322/0040`; hardware only |

Test: **hardware only** for all these engines; stock QEMU has no matching
NVIDIA 2D silicon model. A VRAM command pushbuffer is not permission to expose
arbitrary packets, addresses or bus mastering. The lead must explicitly
accept its reserved extent, validation, reset and bounded completion design
before that transport is enabled. VBE first; FX/6 do not inherit a GF2 pass.

### Matrox G200 / G400 / G450 / G550 — choose L

Vendor `102b`: G200 `0520 0521`, G400/G450 `0525`, G550 `2527`.
Use L `matrox/matroxfb_base.c` table; disambiguate G400/G450 by revision and
source-specific checks rather than assigning one ID to two devices.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `matrox/matroxfb_accel.c`, `matroxfb_base.h`, `matroxfb_accel.h`; cursor from `matroxfb_DAC1064.c`** | GPL-2.0-only / GPL-2.0 headers | F C Y M U*; fill wrapper only implements ROP_COPY | Native; no fbdev video overlay API | **2–3.5 kLOC**, L; 24bpp/mono packing and DAC variants. No I inferred from generic fbdev ROP enums. | `102b:0520/0521/0525/2527`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `matrox/engine/mga_acc.c`, `Cursor.c`, kernel `matrox/driver.c` | Be Sample wrappers compatible; engine/LICENSE has informal permission and an explicit Maven exception: **hold** | F C Y I K* U | Native; BES overlay, separate Maven/TV work | 2–4 kLOC, H; do not label the entire driver MIT. | `102b:0520/0521/0525/2527`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) mga `mga_storm.c`, `mga_hwcurs.c` | no per-file licence header: **hold** | F C Y M W I K* L P U* | Native; video scaler/BES in separate file | 2.5–4.5 kLOC, X; headers and optional HAL not automatically reusable. | `102b:0520/0521/0525/2527`; hardware only |

Test: **hardware only**, with separate G200 and G400/G450/G550 acceptance.
Four pack pairs are useful evidence of presence, not acceleration evidence.

### Intel i740 — choose X i740

Examples `8086:7800` (AGP), `8086:00d1` (PCI); validate exact table and bridge.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[X](#3-snapshot-catalogue-and-notation) i740 `i740_accel.c`, `i740_cursor.c`, `i740_reg.h`, `i740_macros.h`** | MIT-style | F C Y M* I L P U | Native UMS; video path separate | **1.5–3 kLOC**, X; mono expansion has a documented unresolved engine lockup, so initial M stays off. | `8086:7800/00d1`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `i740fb.c`, `i740_reg.h` | GPL-2.0-only core | Software drawing only (`fb_ops:992–1005`) | Native; no exposed overlay | 1.5–3 kLOC mode-only; not a DM2 accelerated donor. | `8086:7800/00d1`; hardware only |
| H | No i740 accelerant/kernel family in r1beta5 | None established | VBE fallback | No host port. | `8086:7800/00d1`; hardware only |

Test: **hardware only**; zero pack models. Do not assume i810 register
compatibility. X `i740_accel.c:391–402` says its extra wait still does not
resolve the mono lockup; a bounded timeout alone is not a hardware fix.

### Intel i810 / i815 — choose L

Vendor `8086`: `7121 7123 7125 1132`; use L i810's explicit PCI table.
These are not the later i915 DRM/GEM engines.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `i810/i810_accel.c`, `i810_main.c`, `i810.h`, `i810_regs.h`** | GPL-2.0 tree notice, compatible | F C Y M I U | Native; no verified fbdev overlay API | **2.5–4.5 kLOC**, L; ring/GTT/cursor memory needs a bounded reservation and restore contract. | `8086:7121/7123/7125/1132`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `intel_810/engine.cpp`, `hooks.cpp`, kernel `intel_810/driver.cpp` | MIT on inspected files | Y only; no drawing or cursor hooks | Native mode code; no verified overlay | 2–4 kLOC mode-only; **not an acceleration donor** despite the directory name. | `8086:7121/7123/7125/1132`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) intel `legacy/i810/i810_accel.c`, `i810_cursor.c` | MIT-style | F C Y M I L P U | Native UMS; video overlay separate | 2.5–4.5 kLOC, X; legacy XAA memory management. | `8086:7121/7123/7125/1132`; hardware only |

Test: **hardware only**; zero pack models. Native GTT/stolen-memory handling
cannot overwrite firmware allocations or use arbitrary system RAM.

### Intel i830 / i845G / i855GM / i865G / i915G — choose H engine

Vendor `8086`: `3577 2562 3582 2572 2582`; primary/secondary and mobile
variants must be distinguished. H's private `intel_extreme.h` and kernel
`intel_extreme.cpp` define the device groups; LI lists the historical fbdev
set. T23's **830MP chipset is not an integrated i830 GPU**: bind the actual
S3 display function on that laptop.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[H](#3-snapshot-catalogue-and-notation) `intel_extreme/engine.cpp`, `commands.h`, `cursor.cpp`, private `intel_extreme.h`; paired kernel** | MIT on inspected donor files | F C Y I U; fill-span hook deliberately null | Native modes; overlay code exists separately | **2.5–4.5 kLOC**, H; primary ring, fences and stolen/GTT reservations are prerequisites. Mono cursor is not ARGB merely because later Intel chips have it. | `8086:3577/2562/3582/2572/2582`; hardware only |
| [LI](#3-snapshot-catalogue-and-notation) `intelfbdrv.c`, `intelfbhw.c` | GPL wrapper; hardware file explicitly dual GPL/MIT-XFree86 | F C Y M I U | Native, with limitations inherited from firmware state | 3–5 kLOC, L; **removed in v6.8**, global `screen_info` dependency. | `8086:3577/2562/3582/2572/2582`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) intel **tag `2.9.1`**, `i830_accel.c`, `i830_uxa.c`, `i830_cursor.c`, `i830_driver.c` | MIT-style on all four inspected files | F C Y I U A*; upload/buffer helpers need a separate closure audit | Native UMS; UXA/batch/libdrm dependencies and separate XVideo overlay | 4–7 kLOC, X; no i810-to-i830 register inference. Not selected. | `8086:3577/2562/3582/2572/2582`; hardware only |

Test: **hardware only**, per integrated generation. Six pack pairs count one
secondary function. Do not treat Haiku's support for newer Intel devices as
permission to enable them under this directive.

### 3dfx Voodoo3 / Voodoo4 / Voodoo5 — choose H

Vendor `121a`: Voodoo3 `0005`, VSA100/Voodoo4/5 `0009`; Banshee `0003`
is an optional related extension. Voodoo1/2 (`0001/0002`) are separate
3D-only devices and are not a primary display driver for this table.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[H](#3-snapshot-catalogue-and-notation) `3dfx/3dfx_draw.cpp`, `3dfx_init.cpp`, `3dfx_cursor.cpp`, `3dfx.h`; kernel `3dfx/driver.cpp`** | MIT; generic `engine.cpp` is Be Sample BSD-style | F C Y I U | Native; video overlay/scaler in separate `3dfx_overlay.cpp` | **1.5–3 kLOC**, H; single-chip initial path only, VSA multi-chip/SLI separately qualified. | `121a:0005/0009`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `tdfxfb.c` | GPL-2.0-only | F C Y M I U | Native; no fbdev video overlay | 2–4 kLOC, L; bpp/pitch and FIFO restrictions. | `121a:0005/0009`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) tdfx `tdfx_accel.c`, `tdfx_hwcurs.c` | no licence grant in those file headers: **hold** | F C Y M W I K* L P U | Native; XVideo overlay separate | 2.5–4.5 kLOC, X; SLI topology and Glide dependencies belong to later 3D work. | `121a:0005/0009`; hardware only |

Test: **hardware only**, separately Voodoo3 and VSA100. No pack models.
No inference that a generic VGA QEMU model implements this engine.

### S3 Trio64 / ViRGE — choose H, separate native backends

Vendor `5333`: Trio `8811 8814 8901`; ViRGE `5631 883d 8a01 8a10` and
mobile `8c01 8c03`; H `s3/driver.cpp:48–87` supplies the explicit table.
Trio64 and Trio64V+ share an ID and require revision discrimination.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[H](#3-snapshot-catalogue-and-notation) `s3/trio64_{draw,init,cursor}.cpp`, `trio64.h`; `virge_{draw,init,cursor}.cpp`, `virge.h`; paired kernel `s3/driver.cpp`** | MIT; common `engine.cpp` Be Sample BSD-style | F C Y I U | Native; no verified overlay hook in this accelerant | **1.2–2.5 kLOC each**, H; distinct engines, preserve panel/clock state. | `5333:8811/8901/5631/8a01`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `s3fb.c` | GPL tree notice | CPU planar helpers, no packed-pixel F/C hardware path | Native; no overlay API | 1.5–3 kLOC mode-only; not accelerated at target 16/24/32bpp. | `5333:8811/8901/5631/8a01`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) s3 `s3_accel.c`, `s3_cursor.c`; s3virge `s3v_accel.c`, `s3v_hwcurs.c` | permissive X11 / MIT respectively | F C Y M W* I K* L* P U | Native UMS; XVideo paths separate | 2–4 kLOC each, X; ViRGE transparent mono and copy-width workarounds, some lines disabled. | `5333:8811/8901/5631/8a01`; hardware only |

Test: **hardware only**. An S3 desktop twin needs actual S3 silicon/model;
QEMU cirrus is useful for generic fallback but cannot qualify Trio/ViRGE.
The pack has no Trio/ViRGE model.

### S3 Savage / ProSavage / SuperSavage — retain Ciuki native owner

Vendor `5333`: Savage3D `8a20`, Savage4 `8a22`, ProSavage `8a25 8a26`,
Twister `8d01 8d02`, ProSavageDDR `8d03 8d04`, SuperSavage `8c2e`
and siblings; source table H `s3/driver.cpp`. The actual T23 target is
`5333:8c2e`. Existing Ciuki acceptance remains ID/revision-specific.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **Existing Ciuki `savage2d` and SuperSavage mode provider, moved by f2-101** | existing GPL-2.0-only kernel code and retained provenance | Current F C Y P W U; later DM2 commands need explicit additions | Existing native SuperSavage provider; no universal family mode claim | **0.5–1.5 kLOC** registry adaptation estimate, no new source transplant; preserve fault/attempt-marker and measured T23 behavior. | `5333:8a22/8a25/8d03/8c2e`; T23 + other hardware |
| [H](#3-snapshot-catalogue-and-notation) `s3/savage_draw.cpp`, `savage.h`, paired `s3/driver.cpp` | MIT | F C Y I U | Native; separate streams/video work | 1.5–3 kLOC alternative, H; upstream unbounded waits must not replace current policy. | `5333:8a22/8a25/8d03/8c2e`; T23 + other hardware |
| [L](#3-snapshot-catalogue-and-notation) `savage/savagefb_accel.c`, `savagefb_driver.c` | GPL tree notices | F C Y M I; generic fbdev software branch also exists | Native; no fbdev overlay API | 2–4 kLOC alternative, L. | `5333:8a22/8a25/8d03/8c2e`; T23 + other hardware |
| [X](#3-snapshot-catalogue-and-notation) savage `savage_xaa.c`, `savage_cursor.c` | cursor MIT; XAA file has no licence grant: **hold** | F C Y M W I K* L P U | Native UMS/VBE choices; streams scaler separate | 3–5 kLOC alternative, X; COB/DMA and device-specific errata need separate review. | `5333:8a22/8a25/8d03/8c2e`; T23 + other hardware |

Test: **T23 hardware**; QEMU does not emulate the Savage engine. Existing
software/fake tests establish host behavior only. One pack model, not a
reason to privilege S3 in generic code.

### VIA UniChrome / UniChrome Pro — choose L

Vendor `1106`: `3122 7205 3108 3118`; also shared Trident-branded
`1023:8400 8500` (different early engine path). Use L `via/viafbdev.c`
match table and `via/share.h`, not the VIA northbridge vendor alone.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `via/viafbdev.c`, `accel.c`, `accel.h`, `share.h`** | GPL-2.0-or-later, compatible under GPLv2 | F C Y M W* I U | Native; no verified fbdev general video overlay API | **2–4 kLOC**, L; UMA extent, pitches and per-generation engine callback. | `1106:3122/7205/3108/3118`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `via/engine/acc.c`, `Cursor.c`, kernel `via/driver.c` | Be Sample wrappers compatible; engine lacks a licence header: **hold** | F C Y I U; keyed/scaled hook cases commented out | Native; BES overlay exists separately | 2–4 kLOC, H; unsupported heads/secondary display must stay off. | `1106:3122/7205/3108/3118`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) openchrome `via_exa.c`, `via_exa_h2.c`, `via_eng_regs.h` | MIT-style | F C Y W; richer composites are not DM2 S/M by inference | UMS; separate XVideo overlay/scaler | 3–5 kLOC, X; EXA/DRI allocation and H2/H6 engine distinction. | `1106:3122/7205/3108/3118`; hardware only |

Test: **hardware only**. Report the two `1023` integrated IDs in one family
only, then resolve early VIA/Trident ownership in the implementation directive.

### SiS 300 / 315 / 330 — choose L, X for optional commands

Vendor `1039`: SiS300 `0300`, 540/630 `5300 6300`, SiS315 `0310 0315 0325`,
330/Xabre `0330`; integrated derivatives `6325 6330`. Use L `sis/sis_main.c`
PCI table and generation dispatch, not a broad `1039` match. `6342` in the
pack is a later derivative, not proof of target-period coverage.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `sis/sis_accel.c`, `sis_accel.h`, `sis.h`; PCI extraction from `sis_main.c`** | GPL-2.0-or-later, compatible | F C Y I; K* internal copy primitive; imageblit is software | Native but substantial BIOS/panel code; no fbdev video API | **2–3.5 kLOC**, L, VBE first; separate 300 and 310/315/330 command handling. | `1039:0300/0315/0330/6325`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) sis `sis300_accel.c`, `sis310_accel.c`, `sis_cursor.c` | BSD-3-Clause-style, compatible | F C Y M* W* I K* L P U A* | Native UMS; video scaler separate | **+2–4 kLOC** optional operations, X; revisions and queues require independent gating. | `1039:0300/0315/0330/6325`; hardware only |
| H | No SiS family in the r1beta5 accelerator/kernel list | None established | VBE | No host port. | `1039:0300/0315/0330/6325`; hardware only |

Test: **hardware only**. Do not copy the entire SiS BIOS interpreter just
to obtain F/C. Never promote an internal keyed primitive without checking
format, overlap and clip semantics.

### Trident Blade / CyberBlade — choose L

Vendor `1023`: Blade3D `9880`, CyberBlade `8400 8420 8500 8520 8600 8620`,
XP `8820`; explicit L `tridentfb.c:1738–1760` and `include/video/trident.h`
are authoritative. Shared VIA-branded IDs above must get a single owner.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `tridentfb.c`, `include/video/trident.h`** | GPL-2.0-only / GPL-2.0 | F C Y M* I*; engine-dependent | Native; no fbdev overlay API | **1.8–3.5 kLOC**, L; Blade/XP/Image/older engines have different operations. | `1023:9880/8400/8500/8820`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) trident `blade_accel.c`, `trident_accel.c` | permissive X11 notice, compatible | F C Y M* W* I L P; cursor in separate file | Native UMS; separate XVideo scaler | 2.5–4.5 kLOC, X; do not use old TGUI commands on Blade or XP. | `1023:9880/8400/8500/8820`; hardware only |
| H | No Trident family in the inspected list | None established | VBE | No host port. | `1023:9880/8400/8500/8820`; hardware only |

Test: **hardware only**. One dedicated XP model plus the two shared
VIA/Trident IDs; not three distinct market-share observations.

### Cirrus Logic GD54xx — choose L

Vendor `1013`, examples GD5430 `00a0`, GD5434 `00a4`, GD5436 `00ac`,
GD5446 `00b8`; older ISA-only GD542x devices have no PCI pair. The baseline
still requires an eligible VBE LFB, which older banked cards may lack.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `cirrusfb.c`, `include/video/cirrus.h`** | GPL tree notice, compatible | F C Y M; no verified W or cursor callback | Native; no overlay API | **1.8–3 kLOC**, L; distinguish PCI GD54xx from Zorro/PPC and later Laguna; bound the busy polls. | `1013:00a0/00a4/00ac/00b8`; QEMU GD5446 + hardware |
| [X](#3-snapshot-catalogue-and-notation) cirrus `alp_xaa.c`, `alp_hwcurs.c` | copyright-only / no licence grant: **hold** | F C Y M W* I L P U | Native UMS; no scaler established from audited files | 2–4 kLOC, X; do not inherit repository-wide permission without resolving files. | `1013:00a0/00a4/00ac/00b8`; QEMU GD5446 + hardware |
| H | No Cirrus family in r1beta5 | None established | VBE | No host port. | `1013:00a0/00a4/00ac/00b8`; QEMU GD5446 + hardware |

Test: lead **`-device cirrus-vga`**, emulating GD5446 only, followed by real
hardware. QEMU model is not evidence for every 54xx revision or video BIOS.

### NeoMagic — choose L core, X cursor optional

Vendor `10c8`: NM2070 `0001`, NM2090 `0002`, NM2093 `0003`, NM2097 `0083`,
NM2160 `0004`, NM2200 `0005`, NM2230 `0025`, NM2360 `0006`, NM2380 `0016`;
verify L `neofb.c:2128–2156` and `include/video/neomagic.h` before using IDs.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[L](#3-snapshot-catalogue-and-notation) `neofb.c`, `include/video/neomagic.h`; optional X `neo_cursor.c`** | GPL tree notice; X cursor permissive X11 | F C Y M W I, engine-dependent; U from X only | Native LCD/CRT code; no fbdev overlay API | **2–3.5 kLOC**, L; older chips fall back to software. Preserve Fn-key/panel state. | `10c8:0001/0005/0025/0006/0016`; hardware only |
| [H](#3-snapshot-catalogue-and-notation) `neomagic/engine/nm_acc.c`, `Cursor.c`, kernel `neomagic/driver.c` | Be Sample wrappers compatible; engine no explicit grant: **hold** | F C Y I U | Native; BES video overlay separate | 2–4 kLOC, H; engine bugs explicitly noted in `nm_acc.c:7–10`. | `10c8:0001/0005/0025/0006/0016`; hardware only |
| [X](#3-snapshot-catalogue-and-notation) neomagic `neo_2200.c`, `neo_cursor.c` | permissive X11, compatible | F C Y M I P U; no keyed copy (`NO_TRANSPARENCY`) | Native UMS; separate video scaler | 2–4 kLOC, X; alternate broken-copy path on certain engines. | `10c8:0001/0005/0025/0006/0016`; hardware only |

Test: **hardware only**. For NM2200, 24bpp mono images narrower than 16 pixels
must punt (`neofb.c:1443–1452`), and 32bpp “guessed” settings do not establish
support. No pack models.

### Chips & Technologies — select X c-code conditionally; header licence gate remains

Vendor `102c`, examples HiQV `00c0 00c4 00c5` (65550/65554/65555),
older `00d8` (65545); preserve exact driver table and panel rules.

| Candidate | Licence verdict | DM2 donor paths | Modes; overlay | Port size / risks | PCI examples / validation path |
| --- | --- | --- | --- | --- | --- |
| **[X](#3-snapshot-catalogue-and-notation) chips `ct_accel.c`, `ct_cursor.c`** | permissive X11/MIT in c files; required `ct_BlitMM.h`/`ct_BltHiQV.h` lack a licence header: **hold import closure** | F C Y M W I L P U, HiQV-dependent | Native UMS; separate XVideo | **2–4 kLOC**, X; resolve header provenance before copying a buildable port, or obtain primary register documentation for a new native definition set. | `102c:00c0/00c4/00c5`; hardware only |
| [L](#3-snapshot-catalogue-and-notation) `chipsfb.c` | GPL tree notice | Generic software drawing only | Native, restricted to 800×600 and 8/16bpp in inspected code; no overlay | 1–2 kLOC mode-only; cannot satisfy accelerated F/C. | `102c:00c0/00c4/00c5`; hardware only |
| H | No C&T family in r1beta5 | None established | VBE | No host port. | `102c:00c0/00c4/00c5`; hardware only |

Test: **hardware only**. This is a documented source-licence gate, not a
claim that acceleration is implemented. Baseline VBE software remains usable
only if the board firmware supplies the required LFB mode.

### QEMU standard VGA — choose software / VBE

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| `1234:1111`; Q `vga-pci.c`, Bochs VBE interface | MIT-style model; no emulator code is required in the kernel | DM4 provides all DM2 operations in software; no native 2D engine is implied. VBE mode setting; no overlay. | Lead `-vga std`; **0 additional engine kLOC** beyond f2-101. Validates generic fallback/ABI, never native acceleration. |

### QEMU cirrus-vga — use the Cirrus port

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| `1013:00b8`; Q `cirrus_vga.c`, L `cirrusfb.c` | MIT emulator reference; GPL donor | Family F/C/Y/M subset subject to emulation; VBE initially, no claimed overlay | Lead `-device cirrus-vga`; **no second driver**. Host model/fixtures + emulator readback can qualify this virtual device, not real Cirrus timing, cursor or every ROP. |

### QEMU ati-vga — use Rage128 / Radeon ports

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| `1002:5046` Rage128 Pro, `1002:5159` RV100; Q `ati.c`, `ati_2d.c` | GPL-2.0-or-later model; MIT/GPL family donors | Emulated fill/copy subset; unsupported commands punt. Native CRTC model, firmware mode path must be checked; no overlay/3D qualification | Lead `-device ati-vga,model=rage128p` / `model=rv100`; **no extra driver**. No M6 alias; never spoof `4c59` or call RV100 a laptop qualification. |

### QEMU vmware-svga — choose small native SVGA FIFO backend

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| `15ad:0405`; X vmware `svga_reg.h` plus Q `vmware_vga.c` as behavior reference | Both inspected interface/model files MIT-style. Old X `vmware.c` / `vmwarecurs.c` headers lack a grant and are excluded. | F C Y U from capability register; CPU upload after sync is software W. Q model contains alpha-cursor decoding but does **not advertise** `SVGA_CAP_ALPHA_CURSOR`; do not infer A. Native SVGA mode registers; no general video scaler/3D claim. | Lead `-vga vmware`; **1.5–3 kLOC**, native I/O + FIFO bounds/ordering, no X host. `vmware_vga.c:899–909` advertises rectangle/cursor capabilities, not fences/3D; completion uses bounded SYNC/BUSY, not an assumed fence opcode. |

### QEMU bochs-display — choose software, native mode provider optional

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| `1234:1111`; Q `bochs-display.c` | GPL-2.0-or-later model reference | Software DM4 only; Bochs DISPI mode registers, no blit engine/overlay | Lead `-device bochs-display`; **0 engine kLOC**, **0.3–0.7 kLOC** optional native mode provider later. Shares PCI ID with standard VGA: inspect BAR/interface and firmware/LFB ownership, never bind by ID alone. Pure framebuffer paths do not become accelerated drivers. |

### VirtualBox VMSVGA — share SVGA protocol, no Oracle implementation import

| PCI / source | Licence verdict | Commands, modes and overlay | Test, size and risk |
| --- | --- | --- | --- |
| VMware-compatible `15ad:0405`; V `DevVGA-SVGA.cpp`; MIT X `svga_reg.h` interface | **V code GPL-3.0-only: incompatible with the kernel, excluded**. MIT protocol header remains eligible. This does not prohibit testing Ciuki in VirtualBox. | Runtime capability negotiation, F/C/Y/U only when implemented and advertised; A and any 3D are independent. Native SVGA modes; no assumed overlay. | **VirtualBox-only test**, not a QEMU device; same native backend + **0.2–0.6 kLOC** compatibility fixtures. Neither VBoxVGA nor VBoxSVGA is automatically VMSVGA. Do not enable every capability because one implementation accepts a command. |

## 5. Host API and shim decision

| Strategy | APIs required if the donor were hosted intact | Estimated common shim | Trade-off / verdict |
| --- | --- | --- | --- |
| One Haiku accelerant host | `create_area`, `clone_area`, `delete_area`, `map_physical_memory`, `get_pci_info`, PCI module/ioctl dispatch, area IDs/shared-info structs, `create_sem`/benaphores, `snooze`, `system_time`, thread/interrupt lifetime, display-mode/DDC and AGP helpers, C++ support | **4–8 kLOC** plus **0.5–2 kLOC per family** and mode/AGP work | Hook table resembles DM2 and batches are useful. Several requested families are absent, i810 has no drawing hooks, and licence gaps occur in engines. Hosting OS semantics does not fix unbounded waits or unsafe direct MMIO. Reject as the universal layer. |
| One Linux fbdev host | `fb_info`, `fb_ops`, `fb_var_screeninfo`, pseudo-palette, `fb_image`/pixmap padding, `ioremap`/`readl`/`writel`, `pci_*`, resource ownership, `request_irq`, spinlocks/mutexes, `udelay`/`msleep`/jiffies, allocation and I2C/EDID/AGP helpers, `cfb_*` | **6–12 kLOC** plus **0.5–2 kLOC per family**, substantially more for mode/DRM infrastructure | Broad 2D donor set, GPL-compatible. fbdev's console ABI does not expose many DM2 optional operations; several drivers use CPU drawing. A fake Linux allocation/interrupt environment is unnecessary for a few engine functions. Reject as the universal layer. |
| X.Org DDX host | `ScrnInfoPtr`, `ScreenPtr`, XAA setup/subsequent state, EXA pixmaps and Prepare/Done refusal, `xf86Map*`, VGA/PCI/int10/VBE, cursor/DDC/XVideo, offscreen allocation, optional libdrm | **8–15 kLOC** plus **1–3 kLOC per family** | Rich ROP/upload/cursor code, but server state and DRI/KMS dependencies are too large; no full DDX host proposed. |
| **Native ports behind DM1–DM13** | Ciuki PCI/resource claims; UC MMIO and explicitly drained WC VRAM access; validated pitch/format; native locks; monotonic clock plus iteration cap; generic fenced offscreen allocation, attempt marker and quarantine | **0.8–1.5 kLOC common adaptation/utilities/tests**, additional to f2-101's registry/software engine; family estimates in §4 | **Recommended.** Rewrite OS glue, port only audited register/packing/engine fragments. Share bounds/polling/test primitives; never share silicon-specific register layouts through generic code. Native mode providers and overlays need later explicit directives. |

The cost estimates are planning ranges, not measured API completeness. A
family port's extra driver-specific shim is approximately **0.2–0.6 kLOC**
(included in §4 estimates); mode/panel/overlay ports add roughly **2–8 kLOC**
per family and often more validation than F/C alone. Figures exclude upstream
Mesa, a Linux DRM compatibility layer or a Windows driver host.

### Mandatory translation rules

1. Preserve DM2 pixel semantics. Convert inclusive Haiku rectangles and XAA
   transfer padding explicitly; do not pass Haiku/X cursor masks straight to
   Ciuki without AND/XOR, bit-order, hotspot, signed-position and palette tests.
   Mono cursor sizes are donor maxima (often 64×64); Ciuki's required 32×32
   remains supported. ARGB size/alpha behavior must be separately advertised.
2. DM2 has 16 clip rectangles and DM3 has bounded batches. Split or punt a
   donor call whose clip, overlap direction, pitch, ROP, bpp or transfer width
   cannot be represented. A hardware overlay is not the implementation of S.
3. Waits return errors, have both a deadline and a frozen-clock iteration cap,
   and respect the presenter's 1 ms completion slices. No imported infinite
   `while`, panic, spin-until-success reset, user MMIO or arbitrary user packet.
4. Only F/C/Y are needed to establish a first accelerated binding. Set an
   optional bit only after host fixtures and the driver hardware check agree
   with DM4. A copyright-approved file is not a working driver.
5. No bus mastering, host DMA, AGP/GART takeover, PLL/panel programming or
   firmware VRAM sizing by destructive probes merely to activate F/C. Intel
   and NVIDIA ring prerequisites need their own reviewed reservation/transport
   design. Keep the existing S3 shared-owner restrictions; do not generalize
   one chip's DMA rules to a different engine silently.
6. On refusal or timeout, use the registry's sync/quarantine/fallback policy;
   a wedged engine is not safe for CPU framebuffer access until the documented
   recovery proves it. No new `panic()` path. Preserve durable attempt markers,
   safe mode/`accel=0`, display generation and delegated presentation authority.

### Errata and source-specific limitations requiring fixtures

Line links use the pinned sources in §9. An upstream comment establishes a
known risk; only a measured target establishes that a workaround is sufficient.

| Source / location | Observed behavior and port consequence |
| --- | --- |
| [H](#3-snapshot-catalogue-and-notation) `ati/mach64_draw.cpp:20–114`, `mach64_util.cpp:15–89` | Engine format/chain/trajectory and FIFO/idle sequencing belong to the chip. Keep the state sequence, replace its waits with native bounded failures; do not import modes just to draw. |
| [H](#3-snapshot-catalogue-and-notation) `ati/rage128_draw.cpp:20–125` | Reset/cache/FIFO and pixel width are distinct from Mach64. Separate implementation and fake register trace, including timeout before a launch. |
| [L](#3-snapshot-catalogue-and-notation) `aty/radeon_accel.c:10–40,172–180` | [X](#3-snapshot-catalogue-and-notation) server can change the local framebuffer base; offset fixup exists. Read and validate the active base/pitch, and do not advertise M/W from its CPU imageblit. |
| [L](#3-snapshot-catalogue-and-notation) `nvidia/nv_accel.c:58–62` | VRAM pushbuffer wrap/prefetch race requires skipped NOP words; test end-of-ring jumps and reserve the padding. |
| [H](#3-snapshot-catalogue-and-notation) `nvidia/engine/nv_acc.c:10–33` | NV40+ PIO FIFO reads stop responding; do not propose the old PIO path for GeForce6. |
| [L](#3-snapshot-catalogue-and-notation) `matrox/matroxfb_accel.c:330–336,499–516` | Fill wrapper only handles copy ROP; non-mono imageblit falls back. Golden tests must reject an unsupported ROP rather than silently do nothing. |
| [X](#3-snapshot-catalogue-and-notation) `i740/i740_accel.c:391–402` | Unresolved mono-data/bitblt lockup despite an extra wait. M stays disabled pending hardware evidence. |
| [H](#3-snapshot-catalogue-and-notation) `intel_810/hooks.cpp:70–85`, `engine.cpp:51–65` | Engine management is not drawing acceleration; sync token counter is zero and idle loop merely expires. Do not claim DM2 F/C/Y completion from hooks alone. |
| [H](#3-snapshot-catalogue-and-notation) `intel_extreme/engine.cpp:108–144,166–185`, `cursor.cpp:18–55` | Queue capacity and cursor plane layout differ from Ciuki. Replace queue waiting and test wrap/alignment/bitplanes. |
| [H](#3-snapshot-catalogue-and-notation) `3dfx/3dfx_draw.cpp:109–145` | Uses source/destination XY to derive overlap direction; preserve both axes in clipping tests. No SLI proof follows from a VSA100 device match. |
| [X](#3-snapshot-catalogue-and-notation) `s3virge/s3v_accel.c:82–103,156–190,478–493` | BLT-width and transparent mono bugs; Bresenham line path disabled in some builds. Punt matching widths/formats, not just “all ViRGE supported”. |
| [H](#3-snapshot-catalogue-and-notation) `s3/virge_draw.cpp:15–21`; L `cirrusfb.c:1719–1727,2585–2589` | Unbounded busy polls: rewrite with error propagation before reuse. |
| [L](#3-snapshot-catalogue-and-notation) `via/accel.c:503–533` | Idle failure is logged after polling; Ciuki must return a failure and quarantine rather than report success. |
| [L](#3-snapshot-catalogue-and-notation) `sis/sis_accel.c:81–134,171–236,340–347` | 300 and 310 have separate direction/base and keyed-copy primitives; key setup is not exposed by a generic fbdev capability. |
| [L](#3-snapshot-catalogue-and-notation) `tridentfb.c:322–336,409–440,638–668` | Engine dispatch and mono expansion are chip-specific, colour input falls back; do not advertise W or assume one ROP path for every Blade/XP. |
| [L](#3-snapshot-catalogue-and-notation) `neofb.c:485–497,1443–1452` | Disabled FIFO-space method and <16-pixel 24bpp mono bug. Keep small transfers in software until independently qualified. |
| [L](#3-snapshot-catalogue-and-notation) `chipsfb.c:95–104`; X `chips/ct_BltHiQV.h:1–12` | Linux mode limits do not prove 32bpp accelerated HiQV; X macro header has no licence notice. Resolve provenance and silicon limits first. |

## 6. Directive sequence (proposal for the lead)

The order uses **pack family coverage**, with explicit exceptions for existing
laptop regression gates and early cheap emulator evidence. These numbers are
proposed reservations, not authority to implement without a written directive.
Every port depends on accepted f2-101 DM1–DM13, a finite source import manifest,
and host fake-register/golden tests before the lead's runtime validation.

| Directive | Deliverable and scope | Rationale / gate |
| --- | --- | --- |
| **f2-103** | Radeon MMIO F/C/Y, RV100/R100 first; existing VBE modes, no CP/GART. Explicit R200/R300/R4xx subgroups remain off until their checks pass. | ATI 197 raw, 153 within the broad R100–R400 census group; QEMU RV100 supplies an inexpensive first virtual subset. |
| **f2-104** | NVIDIA nv core TNT/GeForce256/2/3/4; bounded native transport, no automatic NV40 activation. | NVIDIA requested-lineage 140 pairs; physical evidence required. |
| **f2-105** | NVIDIA FX/GeForce6 transport/object differences, explicitly reviewed VRAM pushbuffer and wrap logic. | FX 40 + GeForce6 51 within the 140, counted once. No claim that a pre-NV40 PIO path suffices. |
| **f2-106** | Radeon R200/R300/R420/X800 qualification and optional XA operations/cursor, separate capabilities and MMIO-only initial path. | Completes the broad ATI family after the common core; no blanket future-Xpress/R500 scope. |
| **f2-107** | Intel i830/i845/i855/i865/i915G ring/GTT reservation then H F/C/Y/I/U. | Six pack functions; UMA safety prerequisite, no hardware emulator. |
| **f2-108** | VIA UniChrome F/C/Y and selected mono/write; resolve shared `1023:8400/8500` with Trident. | Six combined distinct pack pairs, no double counting. |
| **f2-109** | SiS 300 and 315/330 F/C/Y/I with separate tables. | Five pack pairs in 315/derivative lineage; 300 remains required despite zero. |
| **f2-110** | Matrox G200 then G400/G450/G550; L F/C/Y/M and optional DAC cursor. | Four distinct pack pairs. |
| **f2-111** | Native VMware SVGA FIFO, capability negotiation, std/bochs fallback regressions; VMSVGA compatibility fixtures. | Two VMware pairs; emulator testability, no Oracle code. |
| **f2-112** | S3 Savage registry-family completion and generic regression; retain T23 shared owner and attempt marker. | One pack model and required existing laptop. Existing driver migration itself belongs to f2-101, not a new competing port. |
| **f2-113** | Trident Blade/CyberBlade per-engine F/C/Y, preserving the VIA ownership decision. | One dedicated XP pair; shared two already counted in f2-108. |
| **f2-114** | Mach64/Rage Mobility E500 first; broader Mach64 extensions only with their own accepted whitelist. | Required qualified-laptop target overrides zero pack coverage. |
| **f2-115** | Rage128 F/C/Y/I/U, virtual Pro first, physical follow-up. | Zero pack pairs but inexpensive QEMU model evidence. |
| **f2-116** | Cirrus GD5446 virtual F/C/Y first, physical GD54xx subset after it. | Zero pack pairs; helpful generic 1998 test target. |
| **f2-117** | 3dfx Voodoo3 then VSA100 single-chip path. | Zero pack pairs; multi-chip/SLI is a separate gate. |
| **f2-118** | S3 Trio64 and ViRGE as separate drivers. | Zero pack pairs; neither is a Savage register alias. |
| **f2-119** | Intel i810/i815, then separate i740 backend with mono disabled. | Zero pack pairs; different ring/UMA versus i740 PIO rules. |
| **f2-120** | Riva128/NV3 independent backend. | Zero pack pairs; not an NV4 match-table addition alone. |
| **f2-121** | NeoMagic per-engine subset and LCD/cursor qualification. | Zero pack pairs; documented small-mono failure. |
| **f2-122** | C&T HiQV native F/C/Y only after the header provenance gate closes. | Zero pack pairs; otherwise VBE software and an explicitly open acceleration gate. |

The lead may move the E500 directive earlier to protect that required machine;
that is an explicit validation priority, not a claim about market share.
Native mode providers, video overlays and hardware S/ARGB additions follow
only when the particular family contract names their source closure and tests.

## 7. Mesa input for F7 (research, no port approval)

All entries below are pinned to M21 or M7, not a moving “latest Mesa”.
R100's source directory is named **`radeon`**; `nouveau_vieux` is built from
**`src/mesa/drivers/dri/nouveau`**; Gallium `i915g` is **`src/gallium/drivers/i915`**.
These names are not interchangeable binaries. Mesa drivers are not isolated
register libraries: they expect core Mesa, shader/compiler code, libc/C++,
loader, winsys and kernel memory/submission services. A 2D fbdev port does
not provide those services.

The estimates below are driver-directory **upstream LOC ranges**, obtained
from Git subtree byte totals divided by approximately 30–45 bytes per line
and rounded outward (comments, generated headers and build files included).
They are deliberately estimates, not `cloc` measurements. Common Mesa,
Gallium/util/NIR and libdrm are excluded and can add **hundreds of thousands
of lines**. “Native service” is additional estimated kernel/winsys work,
not a ready Linux DRM port. Exact audited anchor and service files are in §9.

| Driver / requested hardware | Exact paths and inspected anchor | Licence verdict / driver estimate | Required kernel/DRI services and additional port estimate |
| --- | --- | --- | --- |
| **M21 classic r100** — Radeon R100/RV100/RV200 | `src/mesa/drivers/dri/radeon/`, `radeon_context.c` | Anchor MIT-style; directory 710,349 bytes, **16–24 kLOC**, plus common Radeon/Mesa | Radeon GEM BO create/map/domain, relocations, command validation/submission, tiling, busy/wait/fences, VRAM/GART, DRI2 drawable buffers; **8–15 kLOC** native service initially, excluding whole DRM. No R200 inference. |
| **M21 classic r200** — R200/RV250/RV280 | `src/mesa/drivers/dri/r200/r200_context.c` | MIT-style with preserved Weather Channel notice; 552,617 bytes, **13–19 kLOC**, plus common Radeon | Shared Radeon GEM/CS winsys with generation-specific validation, texture layout and state; **+3–6 kLOC** over an accepted Radeon service, not a new independent owner. |
| **M21 classic i915** — gen2 i830/845/855/865 and gen3 i915/945 | `src/mesa/drivers/dri/i915/{i830_context.c,i915_context.c,intel_screen.c}` | Inspected `i915_context.c` MIT-style; 745,180 bytes, **17–25 kLOC**; remaining closure requires audit | i915 GEM BOs, GTT/aperture accounting, mmap/domain, execbuffer with relocations, tiling and synchronization, getparam/device info, DRI2; **8–15 kLOC**. Unlike i915g, the classic directory includes i830 state. |
| **M21 nouveau_vieux** — TNT/NV4 through GeForce4/NV20 lineage | `src/mesa/drivers/dri/nouveau/{nouveau_context.c,nv04_*,nv10_*,nv20_*}` | Context anchor MIT-style; 640,616 bytes, **15–22 kLOC** | Nouveau object/channel allocation, pushbuffers, BO/GEM, relocations, notifier/fence waits, VRAM/GART residency and DRI2 buffers; **10–20 kLOC**. Does not cover NV3/Riva128 or GeForce6. |
| **M7 DRI1 mga** — G200/G400 lineage | `src/mesa/drivers/dri/mga/{mga_xmesa.c,mgaioctl.c}` | Both anchors MIT-style; 350,578 bytes, **8–12 kLOC** | DRM hardware lock/SAREA/drawable clip lists, DMA buffers, AGP heaps, state dispatch, texture aging, IRQ/vblank/swap; `drm/mga/mga_drv.c` reference. **5–10 kLOC** native service. G450/G550 need explicit IDs/state evidence. |
| **M7 DRI1 r128** — Rage128/M3/Pro | `src/mesa/drivers/dri/r128/{r128_context.c,r128_ioctl.c}` | Both MIT-style; 312,222 bytes, **7–11 kLOC** | CCE init/idle/reset, DMA command/vertex buffers, AGP/maps, lock/SAREA, texture heaps, IRQ/vblank/swap; `drm/r128/r128_drv.c`. **5–10 kLOC**. QEMU Rage128 2D is not a CCE/3D test model. |
| **M7 DRI1 savage** — Savage3D/Savage4 and SuperSavage branches | `src/mesa/drivers/dri/savage/{savage_xmesa.c,savageioctl.c}` | VIA/S3 MIT-style anchors; 316,478 bytes, **7–11 kLOC** | SAREA/lock, validated BCI/DMA buffer submission, state/clip validation, event waits, VRAM/AGP heaps, swaps; `drm/savage/savage_drv.c`. **4–8 kLOC** if rewritten around the existing native owner. SuperSavage vertex-DMA exclusion must survive. |
| **M7 DRI1 tdfx** — Banshee/Voodoo3/VSA100 | `src/mesa/drivers/dri/tdfx/{tdfx_context.c,tdfx_screen.c}` | Mesa anchors MIT-style; 534,947 bytes, **12–18 kLOC**. **Glide dependency has a separate unaudited licence closure** | libglide3 hardware/DRI hooks, MMIO/FB maps, lock/SAREA and shared state, FIFO setup, swap and texture heaps; old tdfx DRM mostly maps/lock, not a complete submission validator. **6–12 kLOC plus Glide**. Multi-chip capability is not automatic. |
| **M7 DRI1 sis** — SiS300-era, not blanket 315/330 | `src/mesa/drivers/dri/sis/sis_context.c` | MIT-style context anchor; 294,221 bytes, **7–10 kLOC** | DRM maps/lock/SAREA, FB and AGP alloc/free heaps, serial/idle synchronization and texture management; `drm/sis/sis_drv.c`. **5–10 kLOC**; 315/330 2D support does not establish Mesa 3D support. |
| **M7 DRI1 unichrome** — CLE266/KM400/K8M800-era | `src/mesa/drivers/dri/unichrome/{via_context.c,via_ioctl.c}` | VIA/S3 MIT-style anchors; 331,062 bytes, **8–12 kLOC** | VIA lock/SAREA, VRAM/AGP heaps, command-buffer validation/submission, DMA/blit/event/IRQ and swap services; D62 `via/via_dri1.c`. **6–12 kLOC**. Check individual UniChrome Pro generation before promising it. |
| **M7 DRI1 i810** — i810/i815 | `src/mesa/drivers/dri/i810/{i810context.c,i810ioctl.c}` | Context MIT-style; **ioctl file lacks a header: hold**; 242,339 bytes, **6–9 kLOC** | i810 AGP/GTT, DMA buffer allocation/dispatch, lock/SAREA, state/clip validation, texture aging, swap/IRQ; `drm/i810/i810_drv.c`. **5–10 kLOC**; no GEM/i915 drop-in. |
| **M21 Gallium r300** — R300/R350/RV3xx/R4xx including X800 | `src/gallium/drivers/r300/r300_screen.c`, `src/gallium/winsys/radeon/drm/radeon_drm_winsys.c` | Inspected anchors MIT-style; driver 1,301,216 bytes, **29–44 kLOC** | Radeon GEM BOs, validated CS/reloc, tiling, fence/wait, VRAM/GTT and winsys synchronization; **8–15 kLOC** common Radeon service + **3–6 kLOC** R3xx/R4xx validation. Much larger than a 2D MMIO port. |
| **M21 Gallium nv30** — GeForce FX/NV30 and GeForce6/NV40 | `src/gallium/drivers/nouveau/nv30/nv30_screen.c` | Screen MIT-style; 510,162 bytes, **12–18 kLOC**; nouveau winsys helper lacks header: hold closure | Nouveau DRM channels/objects, pushbuffers/BO/relocations, residency, fencing, texture layouts, Gallium winsys; **10–20 kLOC** plus shared Nouveau code. No shader/compiler or PCI DMA exposure to untrusted clients. |
| **M21 Gallium i915g** — gen3 i915/945/G33/Pineview | `src/gallium/drivers/i915/i915_screen.c`, `winsys/i915/drm/i915_drm_winsys.c` | Screen MIT-style; winsys helper no header: hold closure; 517,996 bytes, **12–18 kLOC** | i915 GEM mmap/domain/tiling, execbuffer/relocs, aperture and fences, Gallium winsys; **8–15 kLOC**. **Not gen2 i830/i845/i855/i865**; the screen's ID switch enumerates gen3. |
| **M21 Gallium crocus** — normally Intel gen4–7.5 | `src/gallium/drivers/crocus/{crocus_screen.c,crocus_bufmgr.c}` | Both MIT-style; 1,077,311 bytes, **24–36 kLOC** | i915 GEM buffer manager, per-context batch/execbuffer, tiling, sync, getparam/topology/queries, compiler infrastructure; **10–20 kLOC**. Outside this baseline's i915G/gen3. Forced gen8 paths in `crocus_screen.c:750–757` do not make it an i830 driver. |

These are not full per-directory licence clearances. Section 9 audits every
**proposed reusable file in the finite donor set** and marks missing grants;
unread Mesa/common/generated/Glide dependencies are excluded from import.
[The inspected Mesa licence document](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/docs/license.rst)
itself says device drivers are generally MIT and that source files decide;
GLX is SGI Free Software License B, C11 thread emulation is Boost, and extension
headers have Khronos terms. No blanket “Mesa is MIT” verdict applies to a
complete port. A future Mesa directive must enumerate and audit its dependency
closure, preserve notices and choose its loader/winsys. No GPL-3-only fragment
or unresolved file is authorized by this table.

DRI1 services are historical, not the Windows NT NTVDM architecture. Linux's
removal of legacy DRM in v6.3 does not remove L's 2D fbdev donors: for example
[r128 removal 28483b8666bfe7d0ec34cfc492d77e64f97f6de1](https://github.com/torvalds/linux/commit/28483b8666bfe7d0ec34cfc492d77e64f97f6de1)
and [VIA removal 8391e000065d4fac88548e071fc43c3e07cb7047](https://github.com/torvalds/linux/commit/8391e000065d4fac88548e071fc43c3e07cb7047).
D62 is the reproducible retained mainline tag for these references.
Porting the old untrusted SAREA/MMIO userspace contract verbatim would conflict
with Ciuki's shared owner and typed validated submission model.

### Comparison with the existing F7 contract

The existing [F7 contract](f7-3d-acceleration-contract.md) focuses on a native
fixed-function triangle API plus a scalar reference and a first SuperSavage
backend. The later owner direction recorded in AGENTS.md selects **OpenGL**
for the beta. A Mesa public frontend and multiple winsys/backends are a larger
scope than that earlier six-directive sequence. This census does not silently
amend that contract or select a new system architecture.

Keep its valuable invariants: one 2D/3D register owner, fenced VRAM reservations,
process-bound handles, bounded queues, no user-controlled physical addresses,
lease revocation and desktop restoration, honest software fallback identity,
and native-resolution performance gates. Mesa state tracking can be placed
above a validated native service or behind an adapted winsys; the lead must
choose that boundary and update F7 before an implementation directive.

The first F7 hardware family can remain Savage for T23 confirmation, but Mesa
7.11/DRI1 on Savage is not a drop-in replacement for the native MMIO-only
contract. In particular, retain the SuperSavage DMA exclusion, the shared
cursor/2D reservations and unresolved tiling/status gates. Neither Amber nor
Gallium removes the need for register/extent validation. R100/R200, gen2 Intel,
older NVIDIA, SiS315/330, i740, Mach64, Trio/ViRGE and C&T do not all have a
single contemporary Gallium backend; some require old classic drivers, extra
dependencies or software 3D. A universal accelerated 3D promise is unsupported.

## 8. Verification and decisions for the lead

Host verification can establish PCI whitelist selection and active-LFB binding,
register packing, clips/overlap/format conversions, cursor masks, queue bounds,
fence wrap, fault paths, no post-revoke writes and software fallback. Fake MMIO
cannot establish real command completion, BAR alias behavior, video memory
size, monitor timing, errata workarounds, reset safety or physical performance.

The lead's later emulator tests can establish only each implemented model's
subset: std/bochs fallback, Cirrus5446 F/C and ATI Pro/RV100 basic 2D, VMware
FIFO and refusal handling. No named QEMU device qualifies NVIDIA, Matrox,
Intel UMA, Voodoo, Savage, NeoMagic, Trident or C&T. VirtualBox VMSVGA needs
its own VM test. T23 and E500 are mandatory physical regressions; every other
real family needs an available board/firmware/revision and recorded image
SHA-256. Advertised features require DM9 saved-block readback against DM4;
unsupported/untested families stay software. No new hardware or FPS result
is claimed by this document.

Decisions requested at review:

1. Accept the native-port strategy and f2-103…122 proposal, with the E500
   ordering exception available to the lead. It is a plan, not mass activation.
2. Decide the NVIDIA/Intel reserved ring/GTT transport contracts before their
   drivers; do not reinterpret old Haiku PIO code as a solution for NV40.
3. Resolve missing licence notices before any excluded file/dependency is
   copied, especially C&T macro headers and old Mesa winsys/ioctl helpers.
   An alternative explicitly licensed snapshot is acceptable after re-audit.
4. Issue the separate hardware-name extractor fix if complete display naming
   is desired; 202 model IDs are absent from the current JSON. No Windows
   driver payload or literal private metadata is needed for that fix.
5. Cross-review an F7 update defining the Mesa/OpenGL frontend, kernel service
   boundary, allowed transports, full source/licence closure and resource/
   testing budget. Preserve the existing shared-owner and validation gates.

The directive names no host-test command. This research ran the existing
hardware-ID, display-mode and presenter semantic tests sequentially with
`nice -n 19`; all **14 tests passed**. The repository scripts' own existing
ASan/UBSan settings were retained. No global sanitizer option was added.
Exact commands and outputs are summarized in `build/codex/f2-102-report.md`.
No `src/`, diary, build logic or asset changes are part of this contract.
The per-file ledger covers 239 code/header files: 210 compatible donor/reference
files, 28 files with unresolved notices, and one GPL-3.0-only exclusion, plus the separately
reviewed informal Matrox notice. This is a source audit, not a compiled-port result.

## 9. File-level source and licence ledger

This is a **finite source-reuse envelope**, not permission for a whole upstream
folder. The following files' headers were inspected. `Eligible` means compatible
with the GPL-2.0-only kernel when original notices and origin are retained;
it does not mean every line is needed or a complete port is approved. New native
OS glue and state types replace upstream host headers unless explicitly listed.
A follow-up that takes another file must add its licence verdict before copying.
`Hold` excludes a file with no sufficiently explicit grant in its own notice;
a top-level licence might resolve it later, but this research does not assume
that silently. `Excluded` means an explicit incompatible licence.

MIT/X11 and BSD/Be Sample verdicts are based on actual permission conditions;
Be Sample's three conditions are BSD-style and GPLv2-compatible, **not MIT**.
Linux's historical unversioned GPL notices are read with the pinned tree's
GPLv2 COPYING and module declarations; bare SPDX `GPL-2.0` is the historical
GPLv2 identifier, not GPLv3. GPL-2.0-or-later can be used under GPLv2. Retain
additional original permissive notices where Linux incorporates X code.
[Haiku's root licence](https://github.com/haiku/haiku/blob/r1beta5/License.md)
explicitly warns of exceptions, and [Be Sample's exact terms](https://github.com/haiku/haiku/blob/r1beta5/data/system/data/licenses/Be%20Sample%20Code%20License)
were read. No licence labels are inferred from a directory named `mit`.

Each source link is pinned and names the complete upstream path. `Header`
is the line range read for the verdict. Errata links below retain source line
numbers; full source files are only research artifacts, not imported code.

| Inspected upstream file (pinned link) | Header read | Verdict |
| --- | --- | --- |
| [`include/uapi/drm/i915_drm.h`](https://github.com/torvalds/linux/blob/v6.12/include/uapi/drm/i915_drm.h) (v6.12) | 1–80 | Eligible: MIT-style permission |
| [`include/uapi/drm/nouveau_drm.h`](https://github.com/torvalds/linux/blob/v6.12/include/uapi/drm/nouveau_drm.h) (v6.12) | 1–80 | Eligible: MIT-style permission |
| [`include/uapi/drm/radeon_drm.h`](https://github.com/torvalds/linux/blob/v6.12/include/uapi/drm/radeon_drm.h) (v6.12) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/i810/i810_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/i810/i810_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/mga/mga_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/mga/mga_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/r128/r128_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/r128/r128_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/savage/savage_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/savage/savage_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/sis/sis_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/sis/sis_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/tdfx/tdfx_drv.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/tdfx/tdfx_drv.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`drivers/gpu/drm/via/via_dri1.c`](https://github.com/torvalds/linux/blob/v6.2/drivers/gpu/drm/via/via_dri1.c) (v6.2) | 1–80 | Eligible: MIT-style permission |
| [`src/add-ons/accelerants/3dfx/3dfx.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/3dfx/3dfx_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/3dfx/3dfx_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/3dfx/3dfx_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_init.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/3dfx/3dfx_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_mode.cpp) (r1beta5) | 1–80 | Eligible: MIT-style permission |
| [`src/add-ons/accelerants/3dfx/accelerant.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/accelerant.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/3dfx/engine.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/engine.cpp) (r1beta5) | 1–78 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/3dfx/hooks.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/hooks.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/accelerant.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/accelerant.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/cursor.cpp) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/ati/engine.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/engine.cpp) (r1beta5) | 1–78 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/ati/hooks.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/hooks.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_init.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/mach64_util.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_util.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/rage128.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/rage128_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128_cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/rage128_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/rage128_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128_init.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/ati/rage128_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_810/engine.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_810/engine.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_810/hooks.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_810/hooks.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_810/i810_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_810/i810_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_extreme/commands.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/commands.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_extreme/cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_extreme/engine.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/engine.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_extreme/hooks.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/hooks.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/intel_extreme/mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`headers/private/graphics/intel_extreme/intel_extreme.h`](https://github.com/haiku/haiku/blob/r1beta5/headers/private/graphics/intel_extreme/intel_extreme.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/3dfx/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/3dfx/driver.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/ati/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/ati/driver.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/intel_810/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/intel_810/driver.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/intel_extreme/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/intel_extreme/driver.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/intel_extreme/intel_extreme.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/intel_extreme/intel_extreme.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/matrox/driver.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/matrox/driver.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/kernel/drivers/graphics/neomagic/driver.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/neomagic/driver.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/kernel/drivers/graphics/nvidia/driver.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/nvidia/driver.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/kernel/drivers/graphics/radeon/driver.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/radeon/driver.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/kernel/drivers/graphics/s3/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/s3/driver.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/kernel/drivers/graphics/via/driver.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/via/driver.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/matrox/Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/matrox/Cursor.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/matrox/GetAccelerantHook.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/matrox/GetAccelerantHook.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/matrox/engine/mga_acc.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/matrox/engine/mga_acc.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/neomagic/Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/neomagic/Cursor.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/neomagic/GetAccelerantHook.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/neomagic/GetAccelerantHook.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/neomagic/engine/nm_acc.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/neomagic/engine/nm_acc.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/nvidia/Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/Cursor.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/nvidia/GetAccelerantHook.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/GetAccelerantHook.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/nvidia/engine/nv_acc.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/engine/nv_acc.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/nvidia/engine/nv_acc_dma.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/engine/nv_acc_dma.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/nvidia/engine/nv_std.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/engine/nv_std.h) (r1beta5) | 1–17 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/radeon/Acceleration.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/radeon/Acceleration.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/radeon/CP.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/radeon/CP.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/radeon/Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/radeon/Cursor.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/radeon/GetAccelerantHook.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/radeon/GetAccelerantHook.c) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/radeon/SetDisplayMode.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/radeon/SetDisplayMode.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/add-ons/accelerants/s3/accel.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/accel.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/engine.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/engine.cpp) (r1beta5) | 1–78 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/s3/hooks.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/hooks.cpp) (r1beta5) | 1–78 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/savage.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/savage.h) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/savage_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/savage_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/savage_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/savage_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/trio64.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64.h) (r1beta5) | 1–62 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/trio64_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/trio64_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/trio64_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_init.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/trio64_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/trio64_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/virge.h`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge.h) (r1beta5) | 1–78 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/virge_cursor.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_cursor.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/virge_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_draw.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/virge_init.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_init.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/s3/virge_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/virge_mode.cpp) (r1beta5) | 1–80 | Eligible: explicit MIT |
| [`src/add-ons/accelerants/via/Cursor.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/via/Cursor.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/via/GetAccelerantHook.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/via/GetAccelerantHook.c) (r1beta5) | 1–80 | Eligible: Be Sample, BSD-style three conditions |
| [`src/add-ons/accelerants/via/engine/acc.c`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/via/engine/acc.c) (r1beta5) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`drivers/video/fbdev/aty/aty128fb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/aty128fb.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/aty/atyfb.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/atyfb.h) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/aty/atyfb_base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/atyfb_base.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/aty/mach64_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/mach64_accel.c) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/aty/radeon_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeon_accel.c) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/aty/radeon_base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeon_base.c) (v6.12) | 1–80 | Eligible: GPL-2.0 with retained MIT portions |
| [`drivers/video/fbdev/aty/radeonfb.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeonfb.h) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/chipsfb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/chipsfb.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`include/video/cirrus.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/cirrus.h) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/cirrusfb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/cirrusfb.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`include/drm/drm_pciids.h`](https://github.com/torvalds/linux/blob/v6.12/include/drm/drm_pciids.h) (v6.12) | 1–80 | Eligible: SPDX MIT |
| [`drivers/video/fbdev/i740fb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i740fb.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/i810/i810.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810.h) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/i810/i810_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_accel.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/i810/i810_main.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_main.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/i810/i810_regs.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_regs.h) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/intelfb/intelfbdrv.c`](https://github.com/torvalds/linux/blob/v6.7/drivers/video/fbdev/intelfb/intelfbdrv.c) (v6.7) | 1–80 | Eligible: GPL-2.0 wrapper (hardware half separately dual-licensed) |
| [`drivers/video/fbdev/intelfb/intelfbhw.c`](https://github.com/torvalds/linux/blob/v6.7/drivers/video/fbdev/intelfb/intelfbhw.c) (v6.7) | 1–80 | Eligible: GPL-2.0 wrapper / explicit GPL+MIT hardware dual licence |
| [`include/video/mach64.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/mach64.h) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/matrox/matroxfb_DAC1064.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_DAC1064.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/matrox/matroxfb_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_accel.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/matrox/matroxfb_accel.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_accel.h) (v6.12) | 1–9 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/matrox/matroxfb_base.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_base.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/matrox/matroxfb_base.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_base.h) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/matrox/matroxfb_misc.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_misc.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/neofb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/neofb.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`include/video/neomagic.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/neomagic.h) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/nvidia/nv_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/nvidia/nv_accel.c) (v6.12) | 1–80 | Eligible: NVIDIA legacy permissive + explicit GPL-compatibility note |
| [`drivers/video/fbdev/nvidia/nv_hw.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/nvidia/nv_hw.c) (v6.12) | 1–80 | Eligible: NVIDIA legacy permissive + explicit GPL-compatibility note |
| [`drivers/video/fbdev/nvidia/nvidia.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/nvidia/nvidia.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`include/video/radeon.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/radeon.h) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/riva/fbdev.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/riva/fbdev.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/riva/riva_hw.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/riva/riva_hw.c) (v6.12) | 1–80 | Eligible: NVIDIA legacy permissive + explicit GPL-compatibility note |
| [`drivers/video/fbdev/s3fb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/s3fb.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/savage/savagefb_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/savage/savagefb_accel.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/savage/savagefb_driver.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/savage/savagefb_driver.c) (v6.12) | 1–80 | Eligible: historical GPL notice, tree GPLv2 |
| [`drivers/video/fbdev/sis/sis.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/sis/sis.h) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/sis/sis_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/sis/sis_accel.c) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/sis/sis_accel.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/sis/sis_accel.h) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/sis/sis_main.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/sis/sis_main.c) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/tdfxfb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tdfxfb.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`include/video/trident.h`](https://github.com/torvalds/linux/blob/v6.12/include/video/trident.h) (v6.12) | 1–80 | Eligible: GPL-2.0 (historical identifier) |
| [`drivers/video/fbdev/tridentfb.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tridentfb.c) (v6.12) | 1–80 | Eligible: GPL-2.0-only |
| [`drivers/video/fbdev/via/accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/accel.c) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/via/accel.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/accel.h) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/via/global.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/global.h) (v6.12) | 1–72 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/via/share.h`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/share.h) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`drivers/video/fbdev/via/viafbdev.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/viafbdev.c) (v6.12) | 1–80 | Eligible: GPL-2.0-or-later (select GPLv2) |
| [`src/gallium/drivers/crocus/crocus_bufmgr.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/drivers/crocus/crocus_bufmgr.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/drivers/crocus/crocus_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/drivers/crocus/crocus_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/i915/i915_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/i915/i915_context.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/drivers/i915/i915_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/drivers/i915/i915_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/winsys/i915/drm/i915_drm_winsys.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/winsys/i915/drm/i915_drm_winsys.c) (mesa-21.3.9) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/mesa/drivers/dri/i915/intel_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/i915/intel_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/nouveau/nouveau_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/nouveau/nouveau_context.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/nouveau/nouveau_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/nouveau/nouveau_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/winsys/nouveau/drm/nouveau_drm_winsys.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/winsys/nouveau/drm/nouveau_drm_winsys.c) (mesa-21.3.9) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/gallium/drivers/nouveau/nv30/nv30_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/drivers/nouveau/nv30/nv30_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/r200/r200_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/r200/r200_context.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/drivers/r300/r300_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/drivers/r300/r300_screen.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/radeon/radeon_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/mesa/drivers/dri/radeon/radeon_context.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/gallium/winsys/radeon/drm/radeon_drm_winsys.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-21.3.9/src/gallium/winsys/radeon/drm/radeon_drm_winsys.c) (mesa-21.3.9) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/i810/i810context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/i810/i810context.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/i810/i810ioctl.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/i810/i810ioctl.c) (mesa-7.11) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/mesa/drivers/dri/mga/mga_xmesa.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/mga/mga_xmesa.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/mga/mgaioctl.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/mga/mgaioctl.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/r128/r128_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/r128/r128_context.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/r128/r128_ioctl.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/r128/r128_ioctl.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/savage/savage_xmesa.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/savage/savage_xmesa.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/savage/savageioctl.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/savage/savageioctl.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/sis/sis_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/sis/sis_context.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/tdfx/tdfx_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/tdfx/tdfx_context.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/tdfx/tdfx_screen.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/tdfx/tdfx_screen.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/unichrome/via_context.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/unichrome/via_context.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`src/mesa/drivers/dri/unichrome/via_ioctl.c`](https://github.com/chaotic-cx/mesa-mirror/blob/mesa-7.11/src/mesa/drivers/dri/unichrome/via_ioctl.c) (mesa-7.11) | 1–80 | Eligible: MIT-style permission |
| [`hw/display/ati.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/ati.c) (v9.2.0) | 1–80 | Eligible reference: GPL-2.0-or-later model |
| [`hw/display/ati_2d.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/ati_2d.c) (v9.2.0) | 1–80 | Eligible reference: GPL-2.0-or-later model |
| [`hw/display/bochs-display.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/bochs-display.c) (v9.2.0) | 1–80 | Eligible reference: GPL-2.0-or-later model |
| [`hw/display/cirrus_vga.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/cirrus_vga.c) (v9.2.0) | 1–80 | Eligible: MIT-style permission |
| [`hw/display/vga-pci.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/vga-pci.c) (v9.2.0) | 1–80 | Eligible: MIT-style permission |
| [`hw/display/vmware_vga.c`](https://github.com/qemu/qemu/blob/v9.2.0/hw/display/vmware_vga.c) (v9.2.0) | 1–80 | Eligible: MIT-style permission |
| [`src/VBox/Devices/Graphics/DevVGA-SVGA.cpp`](https://github.com/mirror/vbox/blob/74117a1cb257c00e2a92cf522e8e930bd1c4d64b/src/VBox/Devices/Graphics/DevVGA-SVGA.cpp) (74117a1cb257c00e2a92cf522e8e930bd1c4d64b) | 1–80 | Excluded: GPL-3.0-only |
| [`src/radeon_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe/src/radeon_accel.c) (43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_exa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe/src/radeon_exa.c) (43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_exa_funcs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe/src/radeon_exa_funcs.c) (43159ef400c3b18b9f4d3e6fa1c4aef2d60d38fe) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_accel.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_accelfuncs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_accelfuncs.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_cursor.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_driver.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_exa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_exa.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_exa_funcs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_exa_funcs.c) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/radeon_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-ati/blob/xf86-video-ati-6.14.6/src/radeon_reg.h) (xf86-video-ati-6.14.6) | 1–80 | Eligible: MIT-style permission |
| [`src/ct_BlitMM.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/blob/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src/ct_BlitMM.h) (9187d76bd4f8d500fc3403b6d4a7bef55508bf2b) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/ct_BltHiQV.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/blob/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src/ct_BltHiQV.h) (9187d76bd4f8d500fc3403b6d4a7bef55508bf2b) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/ct_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/blob/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src/ct_accel.c) (9187d76bd4f8d500fc3403b6d4a7bef55508bf2b) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/ct_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-chips/blob/9187d76bd4f8d500fc3403b6d4a7bef55508bf2b/src/ct_cursor.c) (9187d76bd4f8d500fc3403b6d4a7bef55508bf2b) | 1–80 | Eligible: MIT-style permission |
| [`src/alp_hwcurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-cirrus/blob/df389885adf71ed3b045c2fde9fd3ba4329e1a58/src/alp_hwcurs.c) (df389885adf71ed3b045c2fde9fd3ba4329e1a58) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/alp_xaa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-cirrus/blob/df389885adf71ed3b045c2fde9fd3ba4329e1a58/src/alp_xaa.c) (df389885adf71ed3b045c2fde9fd3ba4329e1a58) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/i740_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_accel.c) (1f13c18a17914a6e919e604d1dc5589eb18f0141) | 1–80 | Eligible: MIT-style permission |
| [`src/i740_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_cursor.c) (1f13c18a17914a6e919e604d1dc5589eb18f0141) | 1–80 | Eligible: MIT-style permission |
| [`src/i740_macros.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_macros.h) (1f13c18a17914a6e919e604d1dc5589eb18f0141) | 1–60 | Eligible: MIT-style permission |
| [`src/i740_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_reg.h) (1f13c18a17914a6e919e604d1dc5589eb18f0141) | 1–80 | Eligible: MIT-style permission |
| [`src/legacy/i810/i810_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/83b8528773a0b1e6655fa87c9645a0062cee1359/src/legacy/i810/i810_accel.c) (83b8528773a0b1e6655fa87c9645a0062cee1359) | 1–80 | Eligible: MIT-style permission |
| [`src/legacy/i810/i810_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/83b8528773a0b1e6655fa87c9645a0062cee1359/src/legacy/i810/i810_cursor.c) (83b8528773a0b1e6655fa87c9645a0062cee1359) | 1–80 | Eligible: MIT-style permission |
| [`src/i830_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_accel.c) (2.9.1) | 1–80 | Eligible: MIT-style permission |
| [`src/i830_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_cursor.c) (2.9.1) | 1–80 | Eligible: MIT-style permission |
| [`src/i830_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_driver.c) (2.9.1) | 1–80 | Eligible: MIT-style permission |
| [`src/i830_uxa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_uxa.c) (2.9.1) | 1–80 | Eligible: MIT-style permission |
| [`src/atimach64accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mach64/blob/810572536e153ac9e4615a35e2ab99dc266806da/src/atimach64accel.c) (810572536e153ac9e4615a35e2ab99dc266806da) | 1–80 | Eligible: MIT-style permission |
| [`src/atimach64cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mach64/blob/810572536e153ac9e4615a35e2ab99dc266806da/src/atimach64cursor.c) (810572536e153ac9e4615a35e2ab99dc266806da) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/mga_hwcurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mga/blob/8ed77eb7920f375e9490a31f907eccb02fb123cd/src/mga_hwcurs.c) (8ed77eb7920f375e9490a31f907eccb02fb123cd) | 1–40 | Hold: no explicit licence grant in inspected header |
| [`src/mga_storm.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-mga/blob/8ed77eb7920f375e9490a31f907eccb02fb123cd/src/mga_storm.c) (8ed77eb7920f375e9490a31f907eccb02fb123cd) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/neo_2200.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-neomagic/blob/6661bdd4551e4e63e983685464a277845aed3012/src/neo_2200.c) (6661bdd4551e4e63e983685464a277845aed3012) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/neo_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-neomagic/blob/6661bdd4551e4e63e983685464a277845aed3012/src/neo_cursor.c) (6661bdd4551e4e63e983685464a277845aed3012) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/nv_const.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_const.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–25 | Hold: no explicit licence grant in inspected header |
| [`src/nv_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_cursor.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_dma.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_dma.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_driver.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_hw.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_hw.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_local.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_local.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_setup.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_setup.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/nv_type.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_type.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/nv_xaa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_xaa.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_const.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_const.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–8 | Hold: no explicit licence grant in inspected header |
| [`src/riva_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_cursor.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_driver.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_hw.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_hw.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_hw.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_hw.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_tbl.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_tbl.h) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`src/riva_xaa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/riva_xaa.c) (0b6619fec39fada995646fb95bb25e72d3c9a491) | 1–80 | Eligible: MIT-style permission |
| [`external/mit/xf86-video-openchrome/dist/src/via_eng_regs.h`](https://github.com/NetBSD/xsrc/blob/947c2727683984b334ebe249fd981dad3e7a2f44/external/mit/xf86-video-openchrome/dist/src/via_eng_regs.h) (947c2727683984b334ebe249fd981dad3e7a2f44) | 1–80 | Eligible: MIT-style permission |
| [`external/mit/xf86-video-openchrome/dist/src/via_exa.c`](https://github.com/NetBSD/xsrc/blob/947c2727683984b334ebe249fd981dad3e7a2f44/external/mit/xf86-video-openchrome/dist/src/via_exa.c) (947c2727683984b334ebe249fd981dad3e7a2f44) | 1–80 | Eligible: MIT-style permission |
| [`external/mit/xf86-video-openchrome/dist/src/via_exa_h2.c`](https://github.com/NetBSD/xsrc/blob/947c2727683984b334ebe249fd981dad3e7a2f44/external/mit/xf86-video-openchrome/dist/src/via_exa_h2.c) (947c2727683984b334ebe249fd981dad3e7a2f44) | 1–80 | Eligible: MIT-style permission |
| [`src/r128_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-r128/blob/d05022a1c398787d3f21d023a8251cb128ba9f9f/src/r128_accel.c) (d05022a1c398787d3f21d023a8251cb128ba9f9f) | 1–80 | Eligible: MIT-style permission |
| [`src/r128_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-r128/blob/d05022a1c398787d3f21d023a8251cb128ba9f9f/src/r128_cursor.c) (d05022a1c398787d3f21d023a8251cb128ba9f9f) | 1–80 | Eligible: MIT-style permission |
| [`src/s3_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3/blob/ed0fbfd27609e897c42d7ab316481a9504746233/src/s3_accel.c) (ed0fbfd27609e897c42d7ab316481a9504746233) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/s3_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3/blob/ed0fbfd27609e897c42d7ab316481a9504746233/src/s3_cursor.c) (ed0fbfd27609e897c42d7ab316481a9504746233) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/s3v_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_accel.c) (211e2831dcf13c6e5847f6caf894f559d9104c3d) | 1–80 | Eligible: MIT-style permission |
| [`src/s3v_hwcurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_hwcurs.c) (211e2831dcf13c6e5847f6caf894f559d9104c3d) | 1–80 | Eligible: MIT-style permission |
| [`src/savage_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/blob/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src/savage_cursor.c) (d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73) | 1–80 | Eligible: MIT-style permission |
| [`src/savage_xaa.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/blob/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src/savage_xaa.c) (d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/sis300_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-sis/blob/0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92/src/sis300_accel.c) (0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92) | 1–80 | Eligible: BSD-3-Clause-style |
| [`src/sis310_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-sis/blob/0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92/src/sis310_accel.c) (0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92) | 1–80 | Eligible: BSD-3-Clause-style |
| [`src/sis_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-sis/blob/0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92/src/sis_cursor.c) (0f50f8c3db2b9f1c9d4ecab8ad278e9db6418a92) | 1–80 | Eligible: BSD-3-Clause-style |
| [`src/tdfx_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfx_accel.c) (278042ff4c469f24ab7bc28e517e8a78e2821ddc) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/tdfx_hwcurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-tdfx/blob/278042ff4c469f24ab7bc28e517e8a78e2821ddc/src/tdfx_hwcurs.c) (278042ff4c469f24ab7bc28e517e8a78e2821ddc) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/blade_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/blob/64c02f307d7146641adf65e31bcaa2d1238479e7/src/blade_accel.c) (64c02f307d7146641adf65e31bcaa2d1238479e7) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/trident_accel.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-trident/blob/64c02f307d7146641adf65e31bcaa2d1238479e7/src/trident_accel.c) (64c02f307d7146641adf65e31bcaa2d1238479e7) | 1–80 | Eligible: X11-style permission (notice + non-endorsement) |
| [`src/svga_reg.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-vmware/blob/8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89/src/svga_reg.h) (8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89) | 1–80 | Eligible: MIT-style permission |
| [`src/vmware.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-vmware/blob/8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89/src/vmware.c) (8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89) | 1–80 | Hold: no explicit licence grant in inspected header |
| [`src/vmwarecurs.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-vmware/blob/8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89/src/vmwarecurs.c) (8f0cf7c03597532feb1a8ccf3bb1cafcd6170f89) | 1–80 | Hold: no explicit licence grant in inspected header |

Additional non-code notice: [Haiku `matrox/engine/LICENSE`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/matrox/engine/LICENSE), lines 1–9: **hold**, informal attribution permission with a Maven exception; not MIT.


### Explicit exceptions and closed import boundaries

- Haiku NVIDIA/NeoMagic/VIA engine files in the ledger lack a licensing grant
  in their own headers. Their Be Sample wrappers are compatible but do not
  grant rights over a different author's engine automatically.
- Haiku Matrox's `engine/LICENSE` informally permits copying/editing with
  attribution but postpones formal wording and excludes the Maven port.
  Treat this as unresolved, not an MIT or GPL conclusion; use Linux instead.
- Haiku Radeon hooks identify MIT while acceleration/CP/cursor/kernel files
  lack the grant. The inspected hook cannot license the rest by itself.
- X.Org Matrox/3dfx/Cirrus/VMware and Savage XAA have missing or copyright-only
  headers in the named files; preserve the holds even though X.Org packages
  are commonly permissive. The audited alternatives avoid these files.
- X NVIDIA `nv_type.h`/`nv_const.h`/`riva_const.h` and C&T blitter macro headers have no explicit licence
  in their headers. Recreate native OS types; resolve macro-source provenance
  before a C&T transplant. Do not copy header constants under a false MIT label.
- Mesa i915/Nouveau winsys helpers and old i810 ioctl file lack a notice in the
  inspected headers. They are research references, not approved source imports.
  All unlisted shared Mesa, libdrm, compiler/generated and Glide files remain
  outside this finite reuse envelope until audited by an F7 directive.
- VirtualBox `DevVGA-SVGA.cpp` explicitly specifies **GPL-3.0-only**. Its code
  cannot enter the GPL-2.0-only kernel. Reading its protocol behavior and testing
  VMSVGA is distinct from copying its implementation.

### Pinned errata links

- [haiku/ati/mach64_draw.cpp, lines 20–114](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_draw.cpp#L20-L114).
- [haiku/ati/mach64_util.cpp, lines 15–89](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/mach64_util.cpp#L15-L89).
- [haiku/ati/rage128_draw.cpp, lines 20–125](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/ati/rage128_draw.cpp#L20-L125).
- [linux/aty/radeon_accel.c, lines 10–40](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/aty/radeon_accel.c#L10-L40).
- [linux/nvidia/nv_accel.c, lines 58–62](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/nvidia/nv_accel.c#L58-L62).
- [haiku/nvidia/engine/nv_acc.c, lines 10–33](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/nvidia/engine/nv_acc.c#L10-L33).
- [linux/matrox/matroxfb_accel.c, lines 330–336](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/matrox/matroxfb_accel.c#L330-L336).
- [xorg/i740/i740_accel.c, lines 391–402](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-i740/blob/1f13c18a17914a6e919e604d1dc5589eb18f0141/src/i740_accel.c#L391-L402).
- [haiku/intel_810/hooks.cpp, lines 70–85](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_810/hooks.cpp#L70-L85).
- [haiku/intel_extreme/engine.cpp, lines 108–144](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/engine.cpp#L108-L144).
- [haiku/3dfx/3dfx_draw.cpp, lines 109–145](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/3dfx/3dfx_draw.cpp#L109-L145).
- [xorg/s3virge/s3v_accel.c, lines 82–103](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-s3virge/blob/211e2831dcf13c6e5847f6caf894f559d9104c3d/src/s3v_accel.c#L82-L103).
- [linux/cirrusfb.c, lines 1719–1727](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/cirrusfb.c#L1719-L1727).
- [linux/via/accel.c, lines 503–533](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/via/accel.c#L503-L533).
- [linux/sis/sis_accel.c, lines 81–134](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/sis/sis_accel.c#L81-L134).
- [linux/tridentfb.c, lines 638–668](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/tridentfb.c#L638-L668).
- [linux/neofb.c, lines 1443–1452](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/neofb.c#L1443-L1452).
- [linux/chipsfb.c, lines 95–104](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/chipsfb.c#L95-L104).

## 10. Aggregate pack IDs used for family weighting

These public bus pairs are the deduplicated model IDs, not copied descriptions,
subsystem IDs or Windows payloads. Later/other groups stay out of driver matching.

**ATI later/other (44):** `1002:5854`, `1002:5874`, `1002:5954`, `1002:5955`, `1002:5974`, `1002:5a41`, `1002:5a43`, `1002:5a61`, `1002:5a63`, `1002:7100`, `1002:7109`, `1002:710a`, `1002:7120`, `1002:7129`, `1002:712a`, `1002:7140`, `1002:7142`, `1002:7145`, `1002:7146`, `1002:7149`, `1002:714a`, `1002:714d`, `1002:7160`, `1002:7162`, `1002:7166`, `1002:716d`, `1002:7181`, `1002:7183`, `1002:7187`, `1002:71a1`, `1002:71a3`, `1002:71a7`, `1002:71c0`, `1002:71c2`, `1002:71c4`, `1002:71c5`, `1002:71cd`, `1002:71e0`, `1002:71e2`, `1002:71ed`, `1002:7249`, `1002:724b`, `1002:7269`, `1002:726b`.

**Intel i830-i915 (6):** `8086:2562`, `8086:2572`, `8086:2582`, `8086:2782`, `8086:3577`, `8086:3582`.

**Intel later/other (14):** `8086:2592`, `8086:2772`, `8086:2776`, `8086:2792`, `8086:27a2`, `8086:27a6`, `8086:2972`, `8086:2973`, `8086:2982`, `8086:2983`, `8086:2992`, `8086:2993`, `8086:29a2`, `8086:29a3`.

**Matrox (4):** `102b:0520`, `102b:0521`, `102b:0525`, `102b:2527`.

**NVIDIA FX (40):** `10de:00fa`, `10de:00fb`, `10de:00fc`, `10de:00fd`, `10de:00fe`, `10de:00ff`, `10de:0301`, `10de:0302`, `10de:0308`, `10de:0309`, `10de:0311`, `10de:0312`, `10de:0314`, `10de:031a`, `10de:031b`, `10de:031c`, `10de:0320`, `10de:0321`, `10de:0322`, `10de:0323`, `10de:0324`, `10de:0326`, `10de:0327`, `10de:0328`, `10de:032a`, `10de:032b`, `10de:0330`, `10de:0331`, `10de:0332`, `10de:0333`, `10de:0334`, `10de:0338`, `10de:033f`, `10de:0341`, `10de:0342`, `10de:0343`, `10de:0344`, `10de:0347`, `10de:034c`, `10de:034e`.

**NVIDIA GeForce2 (7):** `10de:0110`, `10de:0111`, `10de:0112`, `10de:0113`, `10de:0150`, `10de:0152`, `10de:01a0`.

**NVIDIA GeForce3 (4):** `10de:0200`, `10de:0201`, `10de:0202`, `10de:0203`.

**NVIDIA GeForce4 (32):** `10de:0170`, `10de:0171`, `10de:0172`, `10de:0173`, `10de:0174`, `10de:0176`, `10de:0178`, `10de:0179`, `10de:017a`, `10de:017c`, `10de:0181`, `10de:0182`, `10de:0183`, `10de:0185`, `10de:0186`, `10de:0188`, `10de:018a`, `10de:018b`, `10de:018c`, `10de:01f0`, `10de:0250`, `10de:0251`, `10de:0253`, `10de:0258`, `10de:0259`, `10de:025b`, `10de:0280`, `10de:0281`, `10de:0282`, `10de:0286`, `10de:0288`, `10de:0289`.

**NVIDIA GeForce6 (51):** `10de:0040`, `10de:0041`, `10de:0042`, `10de:0043`, `10de:0044`, `10de:0045`, `10de:0047`, `10de:0048`, `10de:004e`, `10de:00c0`, `10de:00c1`, `10de:00c2`, `10de:00c3`, `10de:00cc`, `10de:00cd`, `10de:00ce`, `10de:00f1`, `10de:00f2`, `10de:00f3`, `10de:00f4`, `10de:00f6`, `10de:00f8`, `10de:00f9`, `10de:0140`, `10de:0141`, `10de:0142`, `10de:0143`, `10de:0145`, `10de:0146`, `10de:0147`, `10de:0148`, `10de:014a`, `10de:014d`, `10de:014e`, `10de:014f`, `10de:0160`, `10de:0161`, `10de:0162`, `10de:0163`, `10de:0165`, `10de:0167`, `10de:0168`, `10de:0211`, `10de:0212`, `10de:0215`, `10de:0218`, `10de:0221`, `10de:0222`, `10de:0240`, `10de:0241`, `10de:0242`.

**NVIDIA TNT/TNT2 (6):** `10de:0020`, `10de:0028`, `10de:0029`, `10de:002c`, `10de:002d`, `10de:00a0`.

**NVIDIA later/other (27):** `10de:0090`, `10de:0091`, `10de:0092`, `10de:009d`, `10de:00f5`, `10de:01d1`, `10de:01d3`, `10de:01d7`, `10de:01d8`, `10de:01dc`, `10de:01dd`, `10de:01de`, `10de:01df`, `10de:0290`, `10de:0291`, `10de:0292`, `10de:0293`, `10de:0294`, `10de:029c`, `10de:029d`, `10de:029e`, `10de:029f`, `10de:0391`, `10de:0392`, `10de:0393`, `10de:0394`, `10de:039e`.

**Radeon R100 (11):** `1002:4136`, `1002:4137`, `1002:4336`, `1002:4337`, `1002:4c57`, `1002:4c58`, `1002:4c59`, `1002:5144`, `1002:5157`, `1002:5159`, `1002:515a`.

**Radeon R200 (23):** `1002:4242`, `1002:4966`, `1002:496e`, `1002:4c66`, `1002:4c6e`, `1002:5148`, `1002:514c`, `1002:514d`, `1002:5834`, `1002:5835`, `1002:5940`, `1002:5941`, `1002:5960`, `1002:5961`, `1002:5964`, `1002:5965`, `1002:5c41`, `1002:5c43`, `1002:5c61`, `1002:5c63`, `1002:5d44`, `1002:5d45`, `1002:7834`.

**Radeon R300/R400 (119):** `1002:3150`, `1002:3154`, `1002:3e50`, `1002:3e54`, `1002:3e70`, `1002:3e74`, `1002:4144`, `1002:4146`, `1002:4147`, `1002:4148`, `1002:4150`, `1002:4151`, `1002:4152`, `1002:4153`, `1002:4154`, `1002:4155`, `1002:4164`, `1002:4166`, `1002:4167`, `1002:4168`, `1002:4170`, `1002:4171`, `1002:4172`, `1002:4173`, `1002:4174`, `1002:4175`, `1002:4a49`, `1002:4a4a`, `1002:4a4b`, `1002:4a4d`, `1002:4a50`, `1002:4a54`, `1002:4a69`, `1002:4a6a`, `1002:4a6b`, `1002:4a6d`, `1002:4a70`, `1002:4a74`, `1002:4b49`, `1002:4b4b`, `1002:4b4c`, `1002:4b69`, `1002:4b6b`, `1002:4b6c`, `1002:4e44`, `1002:4e45`, `1002:4e46`, `1002:4e47`, `1002:4e48`, `1002:4e49`, `1002:4e4a`, `1002:4e4b`, `1002:4e50`, `1002:4e51`, `1002:4e54`, `1002:4e64`, `1002:4e65`, `1002:4e66`, `1002:4e67`, `1002:4e68`, `1002:4e69`, `1002:4e6a`, `1002:4e6b`, `1002:4e71`, `1002:5460`, `1002:5549`, `1002:554a`, `1002:554b`, `1002:554d`, `1002:554e`, `1002:554f`, `1002:5550`, `1002:5551`, `1002:5569`, `1002:556a`, `1002:556b`, `1002:556d`, `1002:556e`, `1002:556f`, `1002:5570`, `1002:5571`, `1002:564b`, `1002:564f`, `1002:5653`, `1002:5657`, `1002:566f`, `1002:5677`, `1002:5b60`, `1002:5b62`, `1002:5b63`, `1002:5b64`, `1002:5b65`, `1002:5b70`, `1002:5b72`, `1002:5b73`, `1002:5b74`, `1002:5b75`, `1002:5d48`, `1002:5d4a`, `1002:5d4d`, `1002:5d4f`, `1002:5d52`, `1002:5d57`, `1002:5d6d`, `1002:5d6f`, `1002:5d72`, `1002:5d77`, `1002:5e48`, `1002:5e4a`, `1002:5e4b`, `1002:5e4c`, `1002:5e4d`, `1002:5e4f`, `1002:5e68`, `1002:5e6a`, `1002:5e6b`, `1002:5e6c`, `1002:5e6d`, `1002:5e6f`.

**S3 Savage (1):** `5333:8c2e`.

**SiS (5):** `1039:0325`, `1039:0330`, `1039:6325`, `1039:6330`, `1039:6342`.

**Trident Blade (1):** `1023:8820`.

**VIA/Trident shared (6):** `1023:8400`, `1023:8500`, `1106:3108`, `1106:3118`, `1106:3122`, `1106:7205`.

**VMware (2):** `15ad:0405`, `15ad:0710`.

**other (1):** `1014:0170`.

## 10. Lead decisions (2026-10-22 23:53)

The lead reviewed this contract; it is binding with these decisions:

1. **Strategy and plan.** The native-port strategy is accepted, with one donor per family as chosen above, and f2-103–f2-122 are allocated.
   - Order: f2-103 (Radeon R100, testable on QEMU `ati-vga`), then **f2-114 (Mach64/Rage Mobility, E500) moved forward as the second port**, then the table order.
   - Ports start after f2-101 part A (registry) merges. Optional commands wait for part C, as the f2-101 amendment requires.
2. **Transports.** The NVIDIA and Intel ring/GTT transport contracts are written as the first part of f2-104/f2-105 and f2-107, and reviewed by the lead before their engines run. Haiku PIO code is not reused for NV40.
3. **Licence holds.** No file with an unresolved notice is copied, and the GPL-3.0-only file is excluded. Chips & Technologies stays on VBE software rendering until its header provenance closes. Old Mesa winsys/ioctl helpers are outside this programme.
4. **Names.** The hardware-name extractor fix is issued as f2-123, so that every display adapter has a name in Device Manager and Control Panel.
5. **3D.** The Mesa/OpenGL update of F7 is deferred: the owner agreed that hardware 3D comes after the 2D families. This contract's section 7 is kept as research input.

### Owner decision on GPL-3.0 (2026-10-22 23:54)

The owner allows GPL-3.0 sources ("includi anche GPL-3"). Item 3 above changes accordingly:
- **VirtualBox `DevVGA-SVGA.cpp`** may be read and cited as the behavioural reference for the VMSVGA backend of f2-111. It is VirtualBox's emulator, the host side of the virtual adapter, so it supplies no driver code; the guest protocol comes from the MIT `svga_reg.h`.
- **User-space programs and tools** may import GPL-3.0 code.
- **Kernel code** stays GPL-2.0-only. A GPL-3.0-only file linked into the kernel could not be distributed together with the GPL-2.0-only parts taken from Linux and ACPICA. Any such case is reported to the owner, with the file named, before it is copied.
