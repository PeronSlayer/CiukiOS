# ATI Mach64 (Rage Pro / LT Pro / Mobility) 3D backend (f7-27)

Source audit updated: **2026-10-28** (`date --iso-8601=seconds` on the workstation). Directive:
[f7-27](../directives/f7-27-ati-mach64.md); umbrella rules: the f7-20
amendment of [f7-3d-acceleration-contract.md](f7-3d-acceleration-contract.md)
(sections A–G). This document plugs the Mach64 family into that model. It is
a source-derived implementation and host-model design, **not a silicon
qualification**: QEMU emulates no Mach64 3D engine and nothing here was run
on hardware. The current tree has the Savage-style `record` hook, not f7-13's
background qualification worker; the integration point is noted below.

## 1. Sources and provenance

No upstream code is vendored. Register offsets, field values and write order
were read in the pinned files below and re-expressed independently
(`src/kernel/drivers/video/mach64/registers3d.h`). Directory paths are below
the lead's read-only cache `build/downloads/f7-sources/`.

| Source | Primary URL | SHA-256 |
| --- | --- | --- |
| Mesa 7.11.2 archive (`src/mesa/drivers/dri/mach64/`) | <https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2> | `57746aa8c60bc6f5596ec6c6c7274de40fa12edce5ec33fe899e704fa00b5357` |
| xf86-video-mach64 6.9.6 | <https://www.x.org/releases/individual/driver/xf86-video-mach64-6.9.6.tar.bz2> | `7a0707c71bb522430f83bb5e9d9ee697e661e35534a3a2d10834f86b327a3c9c` |
| FreeBSD release/8.4.0 `sys/dev/drm/mach64_dma.c` | <https://cgit.freebsd.org/src/plain/sys/dev/drm/mach64_dma.c?h=release/8.4.0> | `b1c723ed2fbd32352582814eae6534e73d9821ad9b0c19ce8aa780ac27dd5bd0` |
| FreeBSD release/8.4.0 `mach64_state.c` | same tree | `7ee8966e784dd6202578c8e7f5a2d92be7234c37939abb073029a564a7722d80` |
| FreeBSD release/8.4.0 `mach64_drv.h` | same tree | `6f45fbe4dab88aaeeb3bb9d6d3503f93598a5bc6b744346c023eac0f0ed13652` |
| FreeBSD release/8.4.0 `mach64_drm.h` | same tree | `55f4d76a5690a77c809d8b0fb309de7452c877383de3f2dc51d72b8727ba9fc7` |
| FreeBSD release/8.4.0 `mach64_drv.c` / `mach64_irq.c` | same tree | `5af946793e54844981a5908fc154f52ce8630b9489db3c58abe885a2ef1426e3` / `15195d9502b8dae531a3855f83aed908a182ab8eafbb25a83891c4d4a9cd05d9` (read, not used) |
| Linux v6.1 `drivers/video/fbdev/aty/atyfb_base.c` | <https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/video/fbdev/aty/atyfb_base.c> | `c475645de3767c60e104865ee199a32a3be72870c95eaca1f4f16a83fc4b55e8` |
| Linux v6.1 `include/video/mach64.h` | <https://raw.githubusercontent.com/torvalds/linux/v6.1/include/video/mach64.h> | `ed74a7e208e44d73605248efe98f2cb991a3b03ec3f5cd8de0ea80324853645a` |
| Linux v6.1 `aty/atyfb.h`, `aty/mach64_accel.c` | same tag | `5ac80f000fbc9dbb11f66fde42cf0f511d835c43a5fadc26fed114de63a83cef`, `9ac733e9ae7ad4e1be3adf4b45f9d9ce5f191b33a0437b0472050620de18c1c2` (2D cross-check only) |

Individual Mesa files read (all from the archive above): `mach64_reg.h`
(`e342737c3d430aa41bf5b797cb70271a4b9c3fabc672dac76cdc802352d6bb09`), `mach64_state.c` (`89071f5faa7e0ca7260d1cadef318983988e4cdd2a63dcd15767087772d4f3bb`), `mach64_tris.c`
(`d18bc0db74e93ea52855c3873d1660ae356f593c2a316f5a8a23a70e2137e9a8`), `mach64_texstate.c` (`a8c7dc92c18ae9b97ea6912da55f491a4f973692baf5c3c7f288ac71fb22e54a`), `mach64_texmem.c`
(`b21b2cba19f44ff7851ef8c658f1d7bf30f76de21b9a6e874fdc2f4970b04267`), `mach64_context.c` (`be193b7dd80d728219912a82f0c217d2678c68a8f61d6dd9a439e24b350024e3`), `mach64_context.h`
(`69cd414b88e29fe20315122110f49faf375c853b01d62bc451850f513417f626`), `mach64_ioctl.c` (`cdfc435a739578527708aecf833404c1965fe0eeea4ef1e01e619141393391d4`), `mach64_tex.c`
(`748ac04049f086b7ad35aeaf0401ddf0f04faeab1ec0a70a2c61eef1a7d5a2f9`), `mach64_native_vbtmp.h` (`c971cb8a854eeecb55020cdf32403ec25d957bcb35cad8fc2a453f945aff8fe5`). X.Org files:
`atimach64render.c` (`49917e425bd3eb3901180e322cb28d74efb725cd09a2125752b43d2d566d8b26`), `atimach64accel.c` (`3c85fcd27ee03b3d5d117573ae89ffd1e66f14273c6d47096f500b32a9db3441`),
`atimach64.c` (`c060f2155a580d25960899f054089d1112e2442b026ee1c8a5831e298ca2b464`), `atimach64io.h` (`d2cb3dd94856cf2d1e2e1aa342d6c1dde6393f9594edaf462373344894a8f5d6`),
`atiregs.h` (`b9d2e2b6d134554623e4f6c07e9b1ea7dbfad312aa4c0eabf019558ab691e167`), `atipreinit.c` (`1e8f6480e7e71cbdb6995f2cf7f2a9a866d8d773ac3b5ec2a7a2a133defd0d57`),
`atiprobe.c` (`b898bd5fd30086c571ecf78f4e1259c0e2a438677ee6f9cf2c35e63d79b4b499`), `atilock.c` (`ad306eb064e576bba981e6cca4ad25f6cf6bd193ca2fc4e2db7c778da834db3c`). Full hashes
also appear in the f7-27 report.

Lead decisions D-F2, D-F6 and D-F7 in
`build/lead-scratch/f7-family-decisions.md` bind this implementation: restore
write-only state from the driver's baseline, verify the actual 2D path after
probes, run sandbox host tests sequentially with `nice -n 19`, and research
the pinned local primary sources. D-F1 (RGB565-only render targets) does not
apply: Mach64 renders ARGB8888 even on an RGB565 desktop. No DMA or firmware
exception is used.

Windows 98 Resource Kit, chapter 30, `wrkc30_3.htm:426-436`, read at the
owner-provided read-only `build/rskt/` location (SHA-256
`51c1d5d87de5321d2ecb04ca0f994f4efc98836bdfc85c580c109c3e674f5a40`),
distinguishes local video-memory heaps from non-local AGP/GART heaps.
Decision: this backend exposes only its claimed local VRAM heap; an AGP PCI
identity never authorizes GART or bus-master setup. No Resource Kit text or
vendor driver payload is copied.

The [DRI ATIMach64 page](https://wiki.freedesktop.org/dri/ATIMach64/)
(cited by the contract) limits support to chips with the triangle setup
engine; Utah-GLX's DMA warning is moot because no DMA is used. The read-only
driver pack in `third_party/` contains ATI binaries and INF files only, no
Rage Pro register documentation; nothing was copied from it. Licences:
Mesa/X.Org MIT, DRM MIT-style, Linux GPL-2.0; all usable, nothing imported.

## 2. Chips and identity

The 2D driver's table is authoritative and unchanged
(`mach64/driver.c` `matches[]`): 1002:4742, 4744, 4749, 4750, 4751 (3D Rage
Pro AGP/PCI variants), 4C4D, 4C4E (Rage Mobility P/M, AGP 2x / AGP),
4C52, 4C53 (Rage Mobility L / P/M PCI). Linux `mach64.h:993-1002`
corroborates the IDs and marks 4751 as limited 3D; the same conservative
Q1/Q2/Q3 qualification is mandatory on every variant. These have the GT-Pro triangle setup
engine that Mesa's driver targets. The 2D gate stays: `CONFIG_CHIP_ID & 0xffff`
must equal the PCI device ID before the 2D owner starts, and the 3D preflight
re-reads it (section 6).

**E500 identity.** The owner's Armada E500 reports 1002:4C4D (Rage Mobility
P/M AGP 2x). Its VRAM size, aperture and 2D lease were **not measured** in
this work; the boot records report them (`[3d] probe phase=identity` gives
PCI ID, revision, BAR0/BAR2, PCI command, raw MEM_CNTL, firmware-reported
VRAM and the effective 2D-validated VRAM ceiling;
`phase=preflight` the scanout size, mode, bpp, cursor page, free/largest
heap and the Q1/Q2 requirements). AGP is never used: textures are read from
local VRAM (`TEX_SRC_LOCAL`, Mesa `mach64_reg.h:325`), the GART/AGP aperture is
not touched and `BUS_CNTL` is never written. Section 7 gives the heap plan for
both 4 MiB and 8 MiB parts; the lead must record the E500's actual values
from the first `3d=probe` boot.

## 3. Register blocks and the auxiliary aperture

Mesa and the DRM number registers from **block 1**: block 1 is
`0x000–0x3ff`, block 0 is `0x400–0x7ff` (`mach64_reg.h:42-50`;
DRM `mach64_drv.h:497-513`). X.Org places block 0 at BAR2+0x400 and block 1 at
`Block0Base - 0x400` (`atipreinit.c:724-726`, `:1939`; `atiprobe.c:222-227`);
Linux does the same for the auxiliary aperture and uses +0xc00 only for the
register page at the tail of the linear aperture (`atyfb_base.c:3449-3459`,
block 1 at `mmio_start - 0x400`, `:2572-2586`).

The CiukiOS 2D driver (Haiku-derived `registers.h`) maps the 4 KiB BAR2 page
and uses **0xc00** for block 0. That is the linear-tail layout applied to the
auxiliary aperture, a discrepancy with Linux and X.Org. The 2D driver only
starts when `CONFIG_CHIP_ID` read at BAR2+0xce0 matches the PCI ID, so a
started 2D owner already proves block 0 decodes at 0xc00. The 3D backend
follows X.Org's `Block1Base = Block0Base - 0x400` relative to that validated
base: every 3D page offset is the Mesa offset + 0x800. Before the first
write the preflight additionally reads `CONFIG_CHIP_ID` at BAR2+0x4e0 (the
X.Org/Linux location) and requires the same value, proving that the page
decodes both 2 KiB halves identically; otherwise it refuses with
`reason=aperture_layout` (read-only). **Open question for the lead:** if the
E500's 2D owner refuses with `chip_id_mismatch`, the 2D driver's 0xc00 base
is wrong for this aperture and must move to 0x400 (a 2D-driver fix outside
this directive).

Block 1 must be decoded: `BUS_CNTL` bit 27 `BUS_EXT_REG_EN`
(`mach64_drv.h:215`; X.Org enables it for VT and later, `atimach64.c:114-115`,
`atilock.c:75-76`; the DRM's MMIO mode keeps it set, `mach64_dma.c:1172-1176`;
Linux's mobility init value `0x7b33a040` has it set, `atyfb_base.c:1441-1443`).
This backend **never writes BUS_CNTL** (it also holds bus-master and retry
controls); if bit 27 is clear it refuses before any write with
`reason=block1_disabled` and records the value. Setting it with
save/restore is a lead decision (open question 2).

Read-only identity/status offsets also have pinned definitions: BUS_CNTL,
FIFO_STAT and GUI_STAT are Mesa `mach64_reg.h:98,198,206` (page offsets
0xca0, 0xf10 and 0xf38). CONFIG_CHIP_ID is Linux `include/video/mach64.h:190`
(block-0 offset 0xe0), combined with `atyfb_base.c:3458-3459` for the two
0x4e0/0xce0 page locations. The existing 2D owner's MEM_CNTL read and cursor
quarantine GEN_TEST_CNTL access are Linux `mach64.h:158,180` (0xcb0/0xcd0 in
the retained block-0 layout). No newly admitted write targets either one.

## 4. The 3D engine as implemented

Facts from the pinned sources; items marked **H-** are hypotheses the sources
leave open, modelled explicitly in the fake engine and **measured by the
boot qualification**, never assumed for admission.

| Feature | Source | Backend use |
| --- | --- | --- |
| Triangle setup: three vertex register groups S,T,W,SPEC_ARGB,Z,ARGB,X_Y; a write to ONE_OVER_AREA (after vertex 3) starts the triangle | `mach64_reg.h:343-372`, `:214-215`; native order `mach64_tris.c:92-113,422-492` (default `MACH64_NATIVE_VTXFMT`, `mach64_context.h:59`); X.Org `atimach64render.c:775-790,859-877` | Exactly this order per triangle; 22 writes (31 with unit 1) |
| X_Y: signed 14.2 fixed point, x in the high half | `mach64_tris.c:434,531-538,578`; `mach64_native_vbtmp.h:47-48,290-292` | Only vertices on the 1/4-pixel grid are admitted, so the encoding is exact |
| ONE_OVER_AREA: float `16/a` for the quarter-unit cross product `a=(x0-x2)(y1-y2)-(y0-y2)(x1-x2)`; sign gives winding | `mach64_tris.c:460-472`; X.Org `:870-877` (±1/(w·h)) | Correctly rounded integer division, sign bit; zero-area triangles are skipped (no fragments, as the reference) |
| Z: 16.16, the 16-bit depth in the integer half ("16.1 to the setup engine") | `mach64_state.c:1048-1051`; `mach64_native_vbtmp.h:49`; `mach64_tris.c:576` | `z << 16`, D16 only |
| Colour: 0xAARRGGBB, Gouraud; flat via SETUP_CNTL | `mach64_native_vbtmp.h:276-281`; `mach64_state.c:592-616` | SETUP_CNTL=0; flat colour is sent to all three vertices (constant plane) |
| Texture coordinates S,T unprojected with `TEX_ST_MULT_W`, W = reciprocal W; unit 1 uses SECONDARY_S/T/W | `mach64_native_vbtmp.h:206-241`; `mach64_reg.h:323,338` | S=u, T=v, W=rhw as IEEE floats (round to nearest even); unit 1 shares W |
| Two exposed units, 1024 maximum, **one uploaded level** | `mach64_context.c:181-206` | Mip filters refused before emission; admitted size 64 before Q3, 256 after (largest sampled size) |
| Unit 0 functions REPLACE / MODULATE / ALPHA_DECAL; `TEX_MAP_AEN` selects texel alpha | `mach64_reg.h:266-278`; `mach64_texstate.c:125-235` | MODULATE (all formats), REPLACE and DECAL (not A8); ADD refused |
| Unit 1 composite: MODULATE only | `mach64_texstate.c:237-283,369-407`; `mach64_reg.h:310-317` | Other unit-1 combines refused |
| Filters: unit 0 `TEX_BLEND_FCN` (min) + `BILINEAR_TEX_EN` (mag); two units force `TEX_BLEND_FCN_TRILINEAR` + `TEX_CACHE_SPLIT`; unit 1 `COMP_BLEND/FILTER_BILINEAR` | `mach64_tex.c:70-94`; `mach64_texstate.c:338-349,383-392` | NEAREST or BILINEAR, min=mag; two units require BILINEAR on unit 0 (**H-TRILINEAR**: trilinear without mips equals bilinear) |
| Wrap/clamp per axis and unit | `mach64_tex.c:44-68`; `mach64_texstate.c:355-364,397-402` | REPEAT and CLAMP_TO_EDGE |
| Texture memory: linear rows of `2^l2w` texels, base in `TEX_<maxlog2>_OFF`, unit 1 in SECONDARY_TEX_OFF; size in TEX_SIZE_PITCH | `mach64_texmem.c:117-123,191-230`; `mach64_texstate.c:366-368,404-406`; DRM `mach64_state.c:146,200-203` | All canonical formats upload as ARGB8888 (Mesa's 32 bpp choice, `mach64_tex.c:143-209`) |
| Depth: Z_CNTL enable, eight functions, write mask; Z_OFF_PITCH like DST_OFF_PITCH | `mach64_reg.h:374-387`; `mach64_state.c:232-286,1071-1075` | All eight functions; D16 linear |
| Alpha test: ALPHA_TST_CNTL, eight functions, 8-bit reference, source TEXEL | `mach64_reg.h:72-96`; `mach64_state.c:58-96,1077-1080` | Admitted only with unit 0 textured by an alpha format, no unit 1, opaque incoming colour (**H-TEXEL**: the test sees unit-0 texel alpha) |
| Blending: SCALE_3D_CNTL source/destination factors, enabled by `ALPHA_FOG_EN_ALPHA`; written alpha is the ALPHA_DST multiplexer | `mach64_reg.h:241-265,84-90`; `mach64_state.c:100-172`; X.Org `atimach64render.c:71-76` ("a multiplexer instead of a blender for the final alpha") | Only pairs whose reference alpha equals a multiplexer input for all data: (ONE,ZERO) = no blend, (ZERO,ONE) with ALPHA_DST_DSTALPHA, (ZERO,ZERO) with ALPHA_DST_ZERO (**H-ALPHA**) |
| Fog | `mach64_state.c:318-367` | Not expressible in the request: never enabled (ALPHA_FOG field 0 or EN_ALPHA) |

Coverage hypothesis **H-COV**: samples at pixel centres with the Ciuki top-left
rule. Mesa's `SUBPIXEL_X=0.0125`, `SUBPIXEL_Y=0.15` (`mach64_context.h:116-122`,
used in `mach64_state.c:624-640`) only bias the truncation of off-grid
window coordinates: for 1/4-pixel inputs `(GLint)((x+0.0125)*4)` and
`(GLint)((y+0.15)*4)` equal `4x` and `4y`, so the backend sends the
reference's coordinates unchanged. Q1 tests edges through sample centres in
every orientation; a different tie rule refuses admission (`coverage=`).

## 5. Command submission and FIFO

CPU MMIO writes through the 2D driver's claimed BAR2 page only
(`m64_render3d_write`, `mach64/engine.c`). No bus-master/BM_* register, no
descriptor ring, no AGP: the DRM's pseudo-DMA path (`mach64_dma.c:1226-1364`)
supplies the model of the register stream and its FIFO pacing, not its DMA.

* **FIFO:** before each group of at most 16 writes, wait until `FIFO_STAT`'s
  16-slot thermometer shows the group free (`slots <= 0x8000 >> n`,
  `mach64_dma.c:63-76`; the 2D driver's `wait_ready` uses the same rule;
  X.Org `atimach64io.h:201-203`). `FIFO_ERR` (bit 31) or an all-ones read is
  a fault (`mach64_reg.h:198-200`).
* **Idle/completion:** FIFO empty, then `GUI_STAT` bit 0 clear
  (`mach64_dma.c:81-98`; `mach64_reg.h:206-207`). The engine has no event or
  tag usable by MMIO; completion is this idle proof after the last write and
  a posted-write drain. Software fences remain the service's
  generation/sequence/ordinal records; nothing is authenticated by the chip.
* **Watchdog:** one 20 ms window (the 2D driver's `M64_WAIT_US`) and a
  262144-read cap (including GUI reads) cover every FIFO wait and the final idle proof of a job,
  so a frozen clock still terminates. The register-group API also caps each
  validated group at 8192 writes, above the maximum 7968-write staging image.
  The presenter stays held
  (`fbdev_render3d_enter` already disabled the 2D pause); CPU uploads are bounded by validated resource dimensions; CPU transfers
  are outside the engine window. Readbacks are chunked (128 pixels per 1 ms
  check). The base retains the presenter through these transfers, like the
  shared reference; f7-13 must provide the common sliced orchestration.
* **Whitelist** (`m64_render3d_writable`, enforced before the first write of
  any group, and independently by the fake engine):

| Page offset (Mesa+0x800) | Register | Values written | Citation |
| --- | --- | --- | --- |
| 0xa40–0xa58, 0xa60–0xa78, 0xa80–0xa98 | VERTEX_1..3 S,T,W,SPEC,Z,ARGB,X_Y | per triangle | `mach64_reg.h:343-372`; DRM verifier range `mach64_state.c:521-526` |
| 0xa9c | ONE_OVER_AREA (after VERTEX_3_X_Y) | `16/a` float | `mach64_reg.h:214`; `mach64_tris.c:112-113,479-480` |
| 0xaa0–0xaa8, 0xb28–0xb3c | VERTEX_n_SECONDARY_S/T/W | unit 1 | `mach64_reg.h:344-367`; `mach64_tris.c:96-103` |
| 0xb04 | SETUP_CNTL | 0 | `mach64_reg.h:283-294`; `mach64_state.c:1126-1129`; X.Org `atimach64render.c:737` |
| 0xd00 / 0xd48 / 0xd80 | DST_, Z_, SRC_OFF_PITCH | `(pitch_px/8)<<22 \| offset/8` | `mach64_reg.h:193,387,300`; `mach64_state.c:752-753,1068-1072`; DRM `mach64_dma.c:1036-1041` |
| 0xd0c / 0xd18 / 0xd8c / 0xd98 | DST_Y_X, DST_HEIGHT_WIDTH, SRC_Y_X, SRC_HEIGHT1_WIDTH1 | 0, `(w<<16)\|h` | `mach64_reg.h:196,192,302,298`; 2D driver fill/copy (`engine.c`) |
| 0xd30 | DST_CNTL | 0x3 (3D), 0x23 (fill/blit) | `mach64_reg.h:179-188`; X.Org `atimach64render.c:730`; 2D driver |
| 0xd4c | Z_CNTL | `EN \| func<<4 \| MASK_EN`, 0 for 2D | `mach64_reg.h:374-386`; DRM `mach64_state.c:278,326,391` |
| 0xd50 | ALPHA_TST_CNTL | `ALPHA_DST_* \| ALWAYS` or `EN \| func \| ref<<16` | `mach64_reg.h:72-94`; `mach64_state.c:1077-1080` |
| 0xdc0–0xde8 | TEX_0..10_OFF | unit-0 base at `TEX_<maxlog2>_OFF` | `mach64_reg.h:304`; DRM `mach64_state.c:146,203`; X.Org `:741` |
| 0xdfc | SCALE_3D_CNTL | `0x0100001b \| FCN \| blend \| light \| filter \| AEN`, 0 for 2D | `mach64_reg.h:222-279`; `mach64_state.c:1082-1102`; DRM `:279,327,392` |
| 0xea8 / 0xeb4 | SC_LEFT_RIGHT / SC_TOP_BOTTOM | target rectangle | `mach64_reg.h:220-221`; `mach64_state.c:440-444` |
| 0xec4 | DP_FRGD_CLR | clear colour / D16 value | `mach64_reg.h:113`; DRM `mach64_state.c:295,343` |
| 0xec8 | DP_WRITE_MASK | all ones (required while blending) | `mach64_reg.h:177`; `mach64_state.c:167,1108` |
| 0xed0 | DP_PIX_WIDTH | ARGB8888 (6) in every field; RGB565 (4) for D16 clears | `mach64_reg.h:152-157,391-401`; `mach64_state.c:1110-1114`; DRM `:289-293,337-341` |
| 0xed4 / 0xed8 | DP_MIX / DP_SRC | 3D `0x70007`/`0x505`; fill `0x70003`/`0x101`; blit `0x70003`/`0x300` | `mach64_reg.h:116-175`; `mach64_state.c:1116-1120`; DRM `:297-301,407-409` |
| 0xf08 | CLR_CMP_CNTL | 0 | `mach64_reg.h:107`; `mach64_state.c:1122` |
| 0xf70 / 0xf74 / 0xf78 | TEX_SIZE_PITCH / TEX_CNTL / SECONDARY_TEX_OFF | log2 sizes; `0x40800200 \| clamps \| composite`; unit-1 base | `mach64_reg.h:341,305-338,282`; `mach64_state.c:1133-1145`; `mach64_texstate.c:366-406` |

Never written: GEN_TEST_CNTL (except the 2D quarantine's cursor hide),
BUS_CNTL, GUI_CNTL, GUI_TRAJ_CNTL, any BM_*/AGP/CRTC/DSP/PLL/MEM/LCD/overlay
register, SCRATCH_REG*, PAT_REG*, CONTEXT_MASK, HOST_*.

Per job the emitted order is: clears (DRM clear sequence,
`mach64_state.c:274-354`, with the 2D driver's `DST_Y_X`/`DST_HEIGHT_WIDTH`
launch), state in the DRM emit order (`mach64_state.c:157-205`, scissor after,
`:129-133`), triangles, then the colour→linear copy (DRM swap order
`:391-426` with the 2D driver's copy registers). Batches carry up to 256
triangles (7936 writes) per job.

## 6. Save, restore and containment

The readable state saved before the first write and restored, then
**verified by readback**, after an idle proof on every path is the set X.Org
saves with `inm()` (`atimach64.c:441-522`) and re-tests for write/read
identity (`atimach64accel.c:293-334`): DST_OFF_PITCH, DST_CNTL, SRC_OFF_PITCH,
SCALE_3D_CNTL, SC_LEFT/RIGHT/TOP/BOTTOM (restored through SC_LEFT_RIGHT and
SC_TOP_BOTTOM as X.Org does, `atimach64accel.c:203-207`), DP_FRGD_CLR,
DP_WRITE_MASK, DP_PIX_WIDTH, DP_MIX, DP_SRC, CLR_CMP_CNTL, TEX_SIZE_PITCH,
TEX_CNTL (bit 23 cache flush masked, `mach64_state.c:947-949`) and
`TEX_<saved level>_OFF`. Every other `TEX_n_OFF` written is read first,
written back, and verified individually. Z_CNTL, ALPHA_TST_CNTL and SETUP_CNTL are not in X.Org's read
set; they are restored to X.Org's 2D baseline 0 (`atimach64accel.c:196-197`,
`atimach64.c:905-910`, `atimach64render.c:737`). Z_OFF_PITCH and
SECONDARY_TEX_OFF are parked on the claimed heap's first page as the driver's
write-only baseline. Vertex input registers remain inert with Z/composite
and the setup engine disabled (X.Org does not restore vertex inputs either,
`atimach64render.c:89-94`). This applies lead decision D-F2 / contract B.4.

After Q1 and after Q2/Q3 (including restored numeric refusals), an independent
2D self-check calls the real `m64_engine.ops.fill_rect` and `copy_rect`
functions on 8x8 scratch rectangles. It preserves the restored 2D pixel
format, redirects only SRC/DST addresses and pitches into the two guarded
scratch surfaces, initializes and verifies both pixel blocks, checks guards,
and restores/readback-verifies state again. The RGB565-desktop path is tested
separately from the ARGB8888 desktop. A failed self-check quarantines both
domains and retains the durable marker/claim. A probe that refused before any
write needs no restoration or destructive self-check. An unproven-idle or
already quarantined engine is never written again, including for a self-check.
Loss without a held presenter guard only closes the two domains in memory;
it emits no cursor or other MMIO write. CPU access stays forbidden until a
normal guarded 2D idle proof succeeds.

Containment rules: refusal before the first write when the FIFO is not
empty, GUI_STAT is busy, a status read is all ones, `BUS_EXT_REG_EN` is
clear, or validation fails. Every engine destination is inside the claimed
heap (DST/Z/SRC/TEX bases come only from validated resources whose offset
and reserved extent are checked against the heap and the 8 MiB offset field)
and inside the target rectangle (scissor). Each allocation has a 4 KiB guard
page on both sides, initialized at first upload and checked after every job.
A writer that cannot be proven idle is never written again: timeout without
an idle proof quarantines both the 2D owner (`m64_quarantine`) and the 3D
device, and the registry claim is quarantined, never released.

## 7. VRAM layout and the 2D lease

The heap starts at the page-aligned end of the scanout lease and ends below
the 2D cursor page (top of MEM_CNTL VRAM, capped at 0x7ff000 like the 2D
driver's aperture-alias rule). The live registry inventory is excluded
page-wise (quarantined reservations too); a truncated inventory leaves no
heap. Only the largest free run is claimed (`"mach64-3d"`) and mapped
uncached. Non-texture surfaces grow from the bottom, the texture pool sits at
the top (`texture_pool_bytes`), LRU eviction and canonical RAM shadows come
from the shared service.

| Allocation | Size |
| --- | --- |
| colour target, linear copy target | `align8(w)*4*h`, page-rounded, + 8 KiB guards each |
| D16 | `align8(w)*2*h`, page-rounded, + 8 KiB guards |
| texture (one level) | `w*h*4`, page-rounded, + 8 KiB guards |
| Q1 minimum | 64×64 colour, D16 and copy + 256 KiB pool = 327680 bytes |
| Q2 full | two mode-sized colours, D16, guards + 256²+128² textures (344064 bytes) |

Plan (2D cursor page claimed at the top of VRAM):

| VRAM | Mode | Heap | Q2 needs | Q2 fits |
| --- | --- | --- | --- | --- |
| 4 MiB | 640×480×16 | 3575808 | 3440640 | yes |
| 4 MiB | 640×480×32 | 2961408 | 3440640 | no |
| 4 MiB | 800×600 (16/32) | 3227648 / 2269184 | 5173248 | no |
| 4 MiB | 1024×768 (16/32) | 2617344 / 1044480 | 8232960 | no |
| 8 MiB | 640×480 (16/32) | 7766016 / 7151616 | 3440640 | yes |
| 8 MiB | 800×600 (16/32) | 7417856 / 6459392 | 5173248 | yes |
| 8 MiB | 1024×768 (16/32) | 6807552 / 5234688 | 8232960 | no |

At 1024×768 no Mach64 configuration can hold a mode-sized target, D16 and
the linear copy beside the scanout; the probe then runs Q1 as silicon
evidence and refuses admission with `reason=fullsize_heap`.

## 8. Admission and capability snapshot

`validate()` has no side effects. Structural errors return `-EINVAL`;
capability refusals return `-EOPNOTSUPP` before any emission, so AUTO selects
the scalar path and HARDWARE_REQUIRED callers (the GL wire) apply their own
whole-context latch. The backend sets the service's `whole_context_fallback`
(a generic opt-in backend policy): after one refused AUTO state the context
stays on software, including when the refused job exceeds the software
work budget and returns `-EAGAIN` for a smaller retry. Refused: vertices off the 1/4-pixel grid; mip filters;
ADD; A8 with REPLACE/DECAL; unit-1 combines other than MODULATE; NEAREST on
unit 0 with two units; blend pairs other than the three above; alpha test
outside its subset; varying W before Q3 or with ratio above 4 or outside
[0.5,256]; textures above 64 (before Q3) or 256; copy into a COLOR resource;
and a depth plane with `|z0-z2|+|z1-z2| >= 32768` when testing or writing
depth (the scalar reference's 16-bit barycentric weights can move such a
plane by up to two levels, so the ±1 D16 tolerance is only provable below
this span).

Capabilities (`query_caps`): zero unless the service is usable
(`qualified()` for the current generation). After Q1+Q2: max size/width/height
of the mode, 768 vertices, `max_pixel_work = 2*w*h` (one mode-sized quad,
the work Q2 completed inside the window; Savage uses 4wh), texture units 2,
max texture 64, D16, ARGB8888, texture formats 63, depth functions 255,
combine modes MODULATE|REPLACE|DECAL, filters NEAREST|BILINEAR,
`AFFINE_EXACT`. After the Q3 sampling group: `PROJECTIVE_BILINEAR`, texture
256; alpha group: alpha functions 255; blend group: blend factors ONE|ZERO.
The same snapshot serves opengl32 and native GL; no PCI ID reaches them.

## 9. Boot qualification Q1–Q4

Opt-in with `3d=probe`, through the 2D driver's `record` hook (worker
context, outside the presenter) until f7-13's budgeted background job is
merged; the lead moves the call there. Before the first write: active
healthy 2D owner, supported 16/32-bpp mode, heap ≥ Q1 minimum, presenter pin,
register preflight (chip ID at 0xce0 and 0x4e0, BUS_EXT_REG_EN, FIFO error),
marker store, registry claim + activation + mapping, scratch allocation and
a fresh durable marker `3d-mach64` keyed by vendor/device
(`C:/system/settings/accel-3d-mach64-1002-%04x.cfg`, declared in the
driver's `attempt_files`, so the existing device-keyed retry clears it). A
retained marker refuses. The marker is cleared only after a restored run.

* **Q1 minimum (64×64):** clear + textured nearest MODULATE triangle with
  D16 LESS write and copy (texture quadrants, depth 16384±1, cleared corner,
  copy equality); a farther triangle rejected by LESS; Gouraud
  red/green/blue modulated by the texel within ±2 levels; two coverage-tie
  triangles (edges through sample centres in all orientations, shared
  diagonal) compared exactly against the scalar reference; full colour/copy
  equality.
* **Q2 mode size:** four scenes against the scalar reference — clear + two
  full-target triangles with two BILINEAR units and D16; a farther triangle;
  REPEAT ×16 with the (ZERO,ONE) blend; 256 small triangles with a depth
  plane inside the provable span. Sampled rows (every 16th and the last),
  all columns: coverage and copy exact, channels ±2, D16 ±1, guards exact.
* **Q3 precision envelope (12 phases):** the f7-11 sample-space oracle
  (`mach64_3d_precision.h`, duplicated per amendment E) on the scenes the
  engine can express — perspective ramps/checkers (W ratio 4, both
  orientations), the filter-fraction sweep (≥192 levels per unit),
  large REPEAT coordinates in both unit orders, CLAMP extremes (sampling
  group); sampled alpha test on one unit (alpha group); the (ZERO,ONE) and
  (ZERO,ZERO) blends (blend group). Tolerances are unchanged; a numeric miss
  refuses that group only (alpha and blend depend on the sampling group).
* **Q4 admission:** an explicit successful-Q1 latch is required before Q2/Q3
  can begin. `qualified` additionally requires the live 2D owner
  to retain the same generation, mode and supported framebuffer format; caps as
  in section 8. Nothing persists a qualified state.

Failure policy: a refusal before the first write or a restored numeric or
budget miss keeps 2D healthy, clears the marker and releases the claim
(`result=blocked`). Guard damage, a failed restoration readback, FIFO/GUI
fault or a writer without idle proof quarantines 2D and 3D, retains the
marker, claim (quarantined), mapping and scratch (`result=failed`). The
whole qualification has one 30 s budget starting at Q1 (including the gap
before Q2); total overrun refuses all hardware admission cleanly. Per-job
precision mismatches may reduce a feature group, but total budget expiry
never grants partial admission.

Records: `[3d] probe phase=identity|preflight|registers|armed`,
`[3d] fullsize result=...`, `[3d] precision result=... alpha=... blend=...`,
`[3d] probe result=... reason=... phase=... register=... expected=...
actual=... guard=... coverage=... packets=... triangles=... clears=...
copies=... alpha=... blend=... perspective=... selfchecks=...
selfcheck_error=... marker=...`. A complete healthy qualification reports
`selfchecks=2 selfcheck_error=0`.

## 10. Quake III and what stays in software

Quake III's set (contract C): two stages or two-pass, ≥256 textures, blends
ONE/ONE, SRC_ALPHA/INV_SRC_ALPHA, DST_COLOR/ZERO, alpha test, D16 LESS/LEQUAL,
mip filters, REPEAT. The Mach64 subset covers D16, all depth functions,
REPEAT, two BILINEAR units (MODULATE on unit 1) and 256 textures; it cannot
express mip chains, ADD, or any of Quake III's blends with ARGB destination
alpha semantics, and it only accepts 1/4-pixel vertex positions.

H1 replay (production GL/GDI/wire through the model, `test_mach64_3d_h1.py`):
640×480 qualifies on the 8 MiB model; 1024×768 does not fit (section 7).
All 3000 (single-pass) and 4600 (multipass) triangles per frame run on the
CPU at both sizes: per 640×480 frame 2544 / 4088 draws are off the
quarter-pixel grid, 456 use ADD and the multipass frame has 56 blend
refusals; the first refusal retires the GL context. GPU draws 0, CPU draws
3000 / 4600, zero context switches. This is the honest result for this
engine: Quake III-class GL content needs either GL-side snapping to the
provider's grid or a coverage/attribute-envelope proof for snapped vertices
(open question 1). Native clients with quarter-pixel geometry (demos, UI
compositing) are admitted.

## 11. Risks

* No silicon run: H-COV, H-TEXEL, H-ALPHA, H-TRILINEAR, depth/colour
  rounding and the filter precision are model choices verified only by Q1–Q3
  on the real chip; any miss reduces claims or refuses.
* The 2D driver's 0xc00 block-0 base differs from Linux/X.Org's 0x400 for the
  auxiliary aperture (section 3); the 3D preflight proves the mirror or
  refuses, but a 2D refusal would block 3D entirely.
* `BUS_EXT_REG_EN` may be clear after the E500 BIOS; then 3D refuses until the
  lead decides on a contained BUS_CNTL write.
* VRAM: 4 MiB parts qualify only at 640×480×16; 1024×768 never qualifies.
* Throughput: synchronous colour/D16 readback after every job (shared design)
  dominates; the Rage Pro fill rate may make a mode-sized quad approach the
  20 ms window; Q2 measures it (a timeout with idle proof refuses cleanly).
* Mesa notes MODULATE with RGBA "should fallback when blending enabled for
  complete compliance" (`mach64_texstate.c:185,248`); blending is restricted
  to the three alpha-exact pairs, and Q3 compares all four channels.

## 12. Open questions for the lead

1. GL coordinate grid: add a family-neutral capability (sampling feature bit
   or caps field, ABI change) so opengl32 snaps vertex positions to the
   provider's grid for both its hardware and software paths, or keep Mach64
   GL content in software. H1 shows 85–89 % of draws off the grid.
2. Whether the backend may set `BUS_EXT_REG_EN` (bit 27 only, read-modify-
   write, verified, restored) when the BIOS leaves it clear.
3. Whether the 2D driver's block-0 base should follow Linux/X.Org (BAR2+0x400)
   for the auxiliary aperture.
4. Destination-alpha semantics: a request flag meaning "alpha is
   don't-care" (XRGB targets) would let the common Quake III blends through
   on this engine; today the native interface defines the alpha channel.
5. Moving the call from the `record` hook to f7-13's background job.
