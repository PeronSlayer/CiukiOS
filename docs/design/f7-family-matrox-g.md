# Matrox G200/G400/G450/G550 hardware 3D backend (f7-25)

Research and design date: 2026-10-26 (`date` checked). This implements the
lead's [f7-25 directive](../directives/f7-25-matrox-g.md) behind the existing
`render3d` universal service ([contract](f7-3d-acceleration-contract.md),
[Savage full-size admission](savage-fullsize-admission.md)). Nothing in this
document is a silicon qualification claim: no card of this family is available
and QEMU emulates none of these 3D engines. Every register value below is taken
from the primary sources in section 1; every behaviour that those sources leave
open is listed in section 9 as a risk and is decided empirically by the boot
qualification, never by assumption at run time.

## 1. Sources and provenance

Downloads live under `build/downloads/f7-sources/` (read-only cache, never
committed). URLs and SHA-256:

| Source | Location | SHA-256 |
| --- | --- | --- |
| Mesa 7.11.2, `src/mesa/drivers/dri/mga/*` | `mesa-7.11.2/Mesa-7.11.2` (archive `MesaLib-7.11.2.tar.bz2`, already pinned by f7-02) | `57746aa8c60bc6f5596ec6c6c7274de40fa12edce5ec33fe899e704fa00b5357` |
| Linux v6.1 `drivers/gpu/drm/mga/mga_drv.h` (raw.githubusercontent.com/torvalds/linux/v6.1/...) | `linux-6.1-mga/mga_drv.h` | `18434839f7530f11e529a629ba4bb06d32883300d9245a55f6e55a2a83b16bdb` |
| Linux v6.1 `drivers/gpu/drm/mga/mga_state.c` | `linux-6.1-mga/mga_state.c` | `d73187226a7b30799b4c750cf2c9ed4781b207a7dd908d4faae2910ac65c6176` |
| Linux v6.1 `drivers/gpu/drm/mga/mga_dma.c` | `linux-6.1-mga/mga_dma.c` | `a28a9a6ebc209e8ccf5c296238626483f33df2df277b9dad415712e09fb76616` |
| Linux v6.1 `drivers/gpu/drm/mga/mga_warp.c` | `linux-6.1-mga/mga_warp.c` | `75a787c02080999754c79917b7f4dca119db501d3f8ab7f7780af0947ce5fa2a` |
| Linux v6.1 `include/uapi/drm/mga_drm.h` | `linux-6.1-mga/mga_drm.h` | `8af9981e1cbabbf1f2ff69db1bbec9da874cbd9a8148b6136705d6f698ad81d6` |
| X.Org xf86-video-mga 2.0.0 (https://www.x.org/releases/individual/driver/xf86-video-mga-2.0.0.tar.gz) | `xf86-video-mga/` | `15b0f4cf3ee22eaefb45d54d1a0bf67ee710a292479a273fe3fd86f9fa802f41` |
| Matrox *MGA-G200 Specification* 10582-MS-0301 (https://old.vgamuseum.info/images/stories/doc/matrox/mga-g200_spec.pdf), 574 pages | `matrox-specs/mga-g200_spec.pdf`, text `g200.txt` (`pdftotext -layout`) | `c851312b53443dfa8c99d1703ee005f56eb8cec20c86fded9fee0da1518c2fe1` |
| Matrox *G400 Specification* (https://old.vgamuseum.info/images/stories/doc/matrox/mga-g400_spec.pdf), 690 pages | `matrox-specs/mga-g400_spec.pdf`, text `g400.txt` | `8722dd6d9e2353e1c2ba39fab1962e69058e665221a9fb89ef1818308c9b82cb` |

Spec citations give the printed page and the `g400.txt` line of the extracted
text (`g400.txt:NNNN`). The driver pack under `third_party/` has no Matrox
3D documentation (checked with `find -iname '*matrox*' -o -iname '*mga*'`);
nothing from it is used. No upstream code is vendored: the Mesa/Linux/X.Org
files are read for register meanings and programming order only.

Repository comparison: the 2D port ([matrox-2d.md](matrox-2d.md),
`src/kernel/drivers/video/matrox/`) owns the control aperture (BAR1, 16 KiB),
the PCI identity, a cursor page at the top of VRAM and the attempt markers.
Its `render3d` family descriptor was `R3D_NOT_IMPLEMENTED`; this change supplies the probe-gated backend. The Savage backend
(`src/kernel/drivers/video/s3savage/savage3d*.c`) is the reference shape:
owner-side claims and markers, `render3d_backend` hooks, guarded VRAM, the
minimum/full-size/precision probes and the whole-context fallback.

## 2. Chips and PCI identities

From the 2D driver's match table (`driver.c`, `matches[]`) and
`matrox-2d.md`; the 3D backend never re-probes PCI:

| PCI ID | Chip | 3D engine differences |
| --- | --- | --- |
| 102b:0520 | G200 PCI | one texture unit, 5 mip origins (TEXORG..TEXORG4 absolute), no `TDUALSTAGE`, 64-entry bus FIFO |
| 102b:0521 | G200 AGP | as above |
| 102b:0525 rev <= 0x80 | G400 | two texture units, 11-level mip chains through `TEXORG1` offsets, `TDUALSTAGE0/1` combiners, 16-entry bus FIFO |
| 102b:0525 rev > 0x80 | G450 | programmed as a G400 (Linux and Mesa use one `MGA_CARD_TYPE_G400` path: `mga_warp.c:59-63`, `mga_xmesa.c:230-235`) |
| 102b:2527 | G550 | programmed as a G400 (same path) |

The design uses only the G200/G400 register subsets that both specifications
document for host-programmed trapezoids; G450/G550 inherit the G400 path and
are gated by the same boot qualification.

Hardware feature inventory (separate from the admitted subset): Mesa
`mga_xmesa.c:502-525` sets one texture unit for G200 and two for G400, with
a 2048×2048 maximum 2D texture size. G400 MACCESS documents Z16, Z32,
Z15/S1 and Z24/S8 (`g400.txt:6401-6413`); this backend exclusively uses D16
and advertises no stencil. `MACCESS.fogen` exists (`g400.txt:6419-6420`),
but fog stays in software GL. Hardware texture formats include ARGB8888,
RGB565, ARGB4444, ARGB1555, AL88/I8/paletted and YCbCr modes
(Mesa `mga_texstate.c:46-56`); this implementation uploads canonical
ARGB8888 for all six API formats. Vertices are CPU-transformed screen-space
XYZ, diffuse RGBA, shared reciprocal W and one/two UV sets; there is no
WARP vertex fetch or hardware transform/lighting claim.

## 3. The 3D engine and the chosen submission model

### 3.1 Why host setup, not the WARP microcode

Mesa and Linux drive these chips through the WARP setup engine: a microcode
blob installed into bus-master memory (`mga_warp.c:47` `mga_warp_install_microcode`,
`mga_warp.c:134` `mga_warp_init`, `MGA_WIADDR`/`MGA_WIADDR2` `mga_drv.h:519`
area), vertices fetched by the secondary DMA channel
(`mga_state.c:648` `MGA_SECADDRESS … | MGA_DMA_VERTEX`). That path needs PCI
bus mastering, a primary DMA ring and the microcode; the 2D port deliberately
enables none of these (`matrox-2d.md`, "never enables PCI bus mastering, DMA,
AGP"). The vendor specifications document a second, complete path: the host
programs the trapezoid edges and every interpolant itself and the drawing
engine rasterizes with opcode `TEXTURE_TRAP` (G400 spec §4.5.5.5 "Texture
Mapping", page 4-43, `g400.txt:16765`; §4.5.5.4 Gouraud, page 4-41,
`g400.txt:16677`; §4.5.6 "Sub-Pixel Trapezoids", page 4-54, `g400.txt:17266`;
same chapter in the G200 spec, `g200.txt:13987`). This backend uses that
documented host path through the same bus FIFO the 2D engine already uses.

### 3.2 Command submission, fences and idle detection

- Every register is a FIFO-attributed MMIO dword in BAR1 (DWGREG0 at 0x1C00,
  DWGREG1 at 0x2C00; `mga_drv.h:568` `MGA_EXEC 0x0100`). A primitive starts
  when the last register is written through the 0x1D00–0x1DFF alias
  (G400 spec §4.5.5 note, page 4-36, `g400.txt:16401`; the 2D port uses
  `M_YDSTLEN|M_EXEC`).
- Bus FIFO occupancy: `FIFOSTATUS.fifocount` and `bempty` (page 3-141,
  `g400.txt:6026`; G200 page 3-110 with a 6-bit count, `g200.txt:4958`). The
  2D engine's bounded `poll()` reserves free entries before every packet; the
  3D path reuses it through two new exported entry points
  (`matrox_engine_reserve`, `matrox_engine_sync`) so one FIFO policy exists.
  Chunks never exceed 16 entries (G400 depth, `matrox-2d.md`).
- Idle proof: `STATUS.dwgengsts` bit 16 (page 3-188, `g400.txt:7565`;
  `mga_drv.h:508`), polled by `mga_do_wait_for_idle` (`mga_dma.c:53`-`61`)
  with the DMA bits that this backend never sets. The 2D `sync_engine`
  (bus FIFO empty, engine not busy, second observation) is the completion
  proof after every job.
- Fences are the software generation/sequence fences of `render3d`; a job is
  complete when `execute` returned with the idle proof, exactly as the Savage.
- Watchdog: one 20 ms engine window per execute slice (`slice_started`), the
  2D poll's own 20 ms/200000-poll bound per reservation, 1 ms chunk budgets
  for CPU uploads/readback. A timeout after proven idle fails the context; a
  timeout without idle proof quarantines the shared owner.
- No interrupts, DMA, AGP, SOFTRAP or pseudo-DMA windows are used.

### 3.3 Trapezoid setup

Registers (G400 spec §4.5.5.1 page 4-36 `g400.txt:16405`; §4.5.6 pages
4-54..57 `g400.txt:17266`; register pages AR0–AR6 3-39..45 `g400.txt:3005`,
SGN 3-172 `g400.txt:7098`, YDST 3-283 `g400.txt:10981`, FXLEFT 3-147
`g400.txt:6180`, LEN 3-151 `g400.txt:6331`; Mesa `mgaregs.h:126`-`162`,
`:454`-`455`, `:495`, `:653`, `:1355`):

| Register | Value (sub-pixel mode) |
| --- | --- |
| `SGN` (0x1C58) | `subpixen`(16) | `centersnap`(15) | `us`(14) | `sse`(13) | `sbe`(12) | `bl`(11) ; `sdxl`/`sdxr`/`sdy`/`scanleft` 0 |
| `YDST` (0x1C90) | y of the initial vertex, signed 16.4 |
| `FXLEFT` (0x1CA8), `FXRIGHT` (0x1CAC) | x of the left/right edge start vertex, signed 16.4 |
| `AR0`, `AR2` (0x1C60, 0x1C68) | left edge dY, dX in 1/16 units, 18-bit signed |
| `AR6`, `AR5` (0x1C78, 0x1C74) | right edge dY, dX in 1/16 units |
| `LEN` (0x1C5C) | number of lines |

A triangle with vertices sorted by y (V0 top) becomes a top trapezoid from V0
with both edges set up (`sse=sbe=1`) and `LEN` = rows whose centre lies in
[y0,y1), then a bottom trapezoid from V1 that re-specifies only the broken
edge (`sbe=1`, `sse=0`, `bl` = broken left) and keeps the shared edge's DDA
state ("continuity points", page 4-36; "brkleft … must be programmed for the
bottom-trap", page 4-37 note). When a non-flat top trapezoid covers no row, the draw refuses before
emission: restarting the shared edge at rounded 16.4 coordinates can change
coverage. A packet differential test reproduces that error, so rounding that
split is not an admitted setup. `centersnap=1` selects OpenGL half-integer sampling
(page 4-54). Coverage therefore follows the scalar top-left rule by
construction only in the host model; the hardware's rounding is qualified by
the minimum and full-size probes (exact coverage is mandatory, contract §8.2).

Vertex coordinates must be multiples of 1/16 pixel (`x & 4095 == 0`); the GL
encoder always produces Q4 positions, other native draws fall back to software.

### 3.4 Interpolants

Register pages: DR0 (3-118 `g400.txt:5284`), DR2/DR3, DR4..DR15 (3-121..129,
`g400.txt:5363`-`5609`), ALPHASTART/XINC/YINC (3-36..38 `g400.txt:2935`),
TMR0–TMR8 (3-230..238 `g400.txt:9364`-`9570`); Mesa `mgaregs.h:339`-`350`,
`:123`-`125`, `:1206`-`1214`.

| Interpolant | Start | x increment | y increment | Format |
| --- | --- | --- | --- | --- |
| Z (ZW16) | DR0 | DR2 | DR3 | signed 17.15 |
| red, green, blue | DR4, DR8, DR12 | DR6, DR10, DR14 | DR7, DR11, DR15 | signed 9.15 |
| alpha | ALPHASTART | ALPHAXINC | ALPHAYINC | signed 9.15 |
| s/w, t/w | TMR6, TMR7 | TMR0, TMR2 | TMR1, TMR3 | signed 12.20 |
| q/w | TMR8 | TMR4 | TMR5 | signed 16.16 |

Start values are evaluated at the initial vertex (FXLEFT, YDST). In sub-pixel
mode with `us=1` "all ALU increments must be programmed with 1/16 precise
values" and "the start value will be multiplied by 16 (integer value) after the
sub-pixel adjustment" (page 4-55, `g400.txt:17300`-area). The backend
provides start values and increments divided by 16 (per-sub-pixel units)
for the engine to snap to the first pixel centre and rescale ("setup encoding
0"). Because the specification does not state whether the increments are
rescaled too, the boot qualification first tries "setup encoding 1" (`us=0`, start values pre-computed by the CPU at the first
pixel centre of the first row under the assumed top-left rule, increments per
pixel). On a restored numerical mismatch it then tries encoding 0. Each encoding
must pass the complete minimum probe; the first that passes is retained for the boot, both failing refuses hardware (section 9).

Texture coordinates are perspective-correct. For each triangle the raw
register values are `Q = round((rhw / max(rhw)) * 2^28)` and
`S = round(u_Q16 * Q / 2^18)` (likewise T). These correspond to effective
fractions of 28 for Q and 26 for S/T; their ratio recovers u after the
register's texture-size adjustment. In the documented 16.16/12.20
interpretation, Q/W is at most 4096 and |S/W| at most 1024.
`TEXWIDTH.tw = log2(width) + (4 - 26 + 28) = log2(width) + 6` and
`rfw = 8 - log2(width) - (28 - 16)` follow the general formulas of page
3-228 (`g400.txt:9304-9311`); height is analogous. Mesa's `ofs` constants
(`mga_texstate.c:181`) belong to its WARP vertex format and are not used.
The accepted reciprocal-W ratio is at most 16, an explicit conservative
implementation bound tested by setup and the precision qualification.
Plane gradients use exact rationals over the Q4 triangle area, rounded once
to the register encoding. Scalar barycentric Q16 weights are a different
representation: the minimum/full-size scenes use ±2 colour and ±1 depth,
while precision qualification uses the shared declared sample-space envelope.

### 3.5 Fragment state

- `DWGCTL` (0x1C00, page 3-132 `g400.txt:5685`; `mgaregs.h:363`-`424`):
  `opcod` TRAP (4) for untextured, TEXTURE_TRAP (6) for textured; `atype` ZI
  (depth write, `mgaregs.h:377`) or I (compare only, `:379`); `zmode` per
  depth function (`mgastate.c:209` mapping, `mgaregs.h:383`-`390`); `bop` 0xC
  (`mgastate.c:1138`); `shftzero`; `clipdis=0`; `solid`, `arzero`, `sgnzero`
  0. Depth test off: `NOZCMP | atype I` (`mgastate.c:963`); depth write
  without test: `NOZCMP | ZI`. `R3D_NEVER` has no `zmode` (`mgastate.c:213`
  falls back) and stays in software.
- `MACCESS` (0x1C04, page 3-152 `g400.txt:6365`): `pwidth` PW32 (2),
  `zwidth` ZW16 (0, `mgaregs.h:504`), `nodither` (`mgaregs.h:519`,
  `mgastate.c:1089`), `fogen` 0.
- `ALPHACTRL` (0x2C7C, page 3-33 `g400.txt:2789`; `mgaregs.h:75`-`121`):
  `srcblendf`/`dstblendf` codes (ZERO 0, ONE 1, DST/SRC_COLOR 2,
  ONE_MINUS_DST/SRC_COLOR 3, SRC_ALPHA 4, ONE_MINUS_SRC_ALPHA 5, DST_ALPHA 6,
  ONE_MINUS_DST_ALPHA 7; `mgastate.c:137`), `alphamode` alpha channel
  (`mgaregs.h:98`, `mgastate.c:1146`), `atmode` + `atref` for the final
  alpha test, with `aten` clear to disable the extra pre-modulation test
  (G400 `g400.txt:2890-2898`, Mesa `mgastate.c:1146-1148`). NEVER
  is `ALT` with reference 0 (
  `mgastate.c:78`-`82`), `alphasel` from-texture for textured draws
  (`mga_texstate.c:224`-area G400 rule) and diffused for Gouraud.
- `PLNWT` 0xFFFFFFFF, `FCOL` unused for 3D (alpha channel mode).
- Clipping: `CXBNDRY` = ((w−1)<<16)|0, `YTOP` = 0, `YBOT` = (h−1)·pitch
  (pages 3-107, 3-285, 3-282; `g400.txt:4981`, `:11054`, `:10954`), set per
  target before every job. The clipper bounds every trapezoid and clear write
  to the target rectangle, which is the VRAM containment argument for
  rasterization (reads are only textures).

### 3.6 Textures

Register pages: TEXCTL 3-210 (`g400.txt:8534`), TEXCTL2 3-215
(`g400.txt:8757`), TEXFILTER 3-217 (`g400.txt:8874`), TEXWIDTH/TEXHEIGHT
3-228/3-219 (`g400.txt:9290`, `:8953`), TEXORG/TEXORG1..4 3-221/3-222
(`g400.txt:9025`, `:9071`), TEXTRANS 3-226 (`g400.txt:9210`), TEXBORDERCOL
3-209, TDUALSTAGE0/1 3-196/3-201 (`g400.txt:7857`, `:8135`); Mesa
`mgaregs.h:1042`-`1214`, `:805`-`990`.

- Format: every texture is uploaded as `TW32` (ARGB8888, `mgaregs.h:1050`)
  from the canonical ARGB shadow that `render3d` already decodes for all six
  contract formats. This keeps RGB565/ARGB4444/L8/A8 expansion exact (the
  scalar replicates low bits; the hardware expansion is undocumented) at the
  cost of up to 2× texture memory. The G200 has no 8-bit alpha formats
  (`g200.txt:6661` table), so one rule serves both chips.
- Layout: linear rows, `tpitchlin=1`, `tpitchext` = width (`mga_texstate.c:167`),
  base 32-byte aligned (`texorg<31:5>`). Level sizes are powers of two, so
  the G400 offset chain `TEXORG + TEXORG1 | TEXORG1>>2 | …` (page 4-46 table
  4-3, `g400.txt:16843`-area; `mgatexmem.c:131`, `:237`-`238` `texorg |
  TO_texorgoffsetsel`, `texorg1 = size of level 0`) equals linear packing.
  The G200 programs absolute level origins TEXORG1..4 (`mgatexmem.c:230`-`235`
  area, G200 branch) and supports five levels (`mga_texstate.c:118`,
  `G200_TEX_MAXLEVELS`).
- Minimum map size is 8 texels (`twmask >= 7`, page 3-228 note); mipmapped
  sampling requires base dimensions that are multiples of 16 and a pitch
  multiple of 16 (page 4-45 notes, `g400.txt:16803`-area). The hardware chain
  therefore stops at the 8×8 level (`mapnb` = usable levels − 1,
  `mgaregs.h:1145`, `:1156`; `mga_texstate.c:186`-`190`). A mip draw is
  admitted only when a conservative bound of its LOD never selects a level
  the chain cannot address (section 5); textures smaller than 8 in either
  dimension and draws that need deeper levels stay in software.
- Filters (`mgatex.c:112`-`137`, `mgaregs.h:1130`-`1138`): NEAREST →
  `nrst/nrst`; BILINEAR → `bilin/bilin`; NEAREST_MIP → `minfilter MM1S`,
  `magfilter nrst`; BILINEAR_MIP → `MM4S` + `bilin`; TRILINEAR → `MM8S` +
  `bilin`; `fthres` 0x10 (`mgatex.c:137`, threshold at rho = 1). The sample
  counts (1S/4S/8S) are the reading Mesa uses; the printed mnemonic table
  (page 3-217) labels MM2S/MM4S the other way round. The boot precision
  phases with BILINEAR_MIP and TRILINEAR decide empirically (section 9).
- Wrap: `clampu`/`clampv` (`mgatex.c:57`, `mgaregs.h:1082`) with
  `borderen=0`, i.e. clamp-to-edge. `TEXTRANS`/`TEXTRANSHIGH` = 0x0000FFFF
  (`mga_state.c:184`-area tex emission) with `takey=1, tamask=0` disables
  transparency (`mgatex.c:321`).
- Dual texturing (G400): `TEXCTL2.dualtex` (`mgaregs.h:1115`); texture-1
  registers are written after `TEXCTL2.map1` (`mgaregs.h:1124`,
  `mga_state.c:184`-`208` `mga_g400_emit_tex1`, G400 spec page 4-49 "Dual
  Texturing is done in two programming steps"). The `MGA_G400_TC2_MAGIC`
  bit 15 that Linux sets (`mga_drv.h:519`) is a reserved bit in the
  specification; it is written exactly as Linux does for the G400 path.
- Combiners (G400, `TDUALSTAGE0/1`): from the scalar reference
  (`render3d_scalar.c` `combine()`), using Mesa's field vocabulary
  (`mga_texstate.c:270`, `:351`, `:448` tables; `mgaregs.h:807`-`897`):

  | Mode | RGB | Alpha |
  | --- | --- | --- |
  | MODULATE | `arg2=diffuse/prevstage`, `sel=mul` | `arg2=diffuse/prevstage`, `sel=mul` |
  | REPLACE | `sel=arg1` (A8: `sel=arg2`) | `sel=arg1` (RGB formats: `sel=arg2`) |
  | DECAL | RGBA: blend trick `arg2=diffuse, alphasel=currtex, alpha1inv, arg1mul=alpha1, blend, arg1add=arg2add=mulout, add, sel=add`; RGB: `sel=arg1`; A8: software | `sel=arg2` |
  | ADD | `arg2=diffuse/prevstage, add, sel=add` (A8: `sel=arg2`) | `arg2`, `sel=mul` |

  G200 (no combiners): REPLACE = decal mode selecting the texel
  (`tmodulate=0`, transparency off, `mga_texstate.c:199`-`260`), MODULATE =
  `tmodulate=1` with `alphasel` modulated/diffused, DECAL (RGB/RGBA) =
  `TEXCTL2.decalblend` true decal; ADD and the A8 variants that select the
  diffuse colour stay in software on the G200.

### 3.7 Clears and copies

- Clear: solid TRAP rectangle, `MGA_DWGCTL_CLEAR` (`mga_drv.h:656`), `FCOL`
  = colour or `z | z<<16`, `DSTORG` = target base, `FXBNDRY`, `YDSTLEN+EXEC`
  (`mga_state.c:480`-`548`). The depth clear fills (w+1)/2 32-bit words per
  row because Z16 rows share the colour buffer's byte pitch (section 4).
- Copy (`R3D_COPY`): BITBLT `MGA_DWGCTL_COPY` (`mga_drv.h:668`) with
  `SRCORG`/`DSTORG`, `AR5` = source pitch, `AR0/AR3` start/end, exactly the
  swap blit of `mga_state.c:568`-`605`. Both colour kinds are linear on this
  family, so the copy target may be `R3D_COLOR` or `R3D_LINEAR_COLOR`.

## 4. VRAM layout beside the 2D lease

`DSTORG` is a 64-byte address (page 3-130, `g400.txt:5610`), `ZORG` a
dword address that "must be loaded with a multiple of 128" (page 3-286,
`g400.txt:11081`), both absolute frame-buffer offsets as Linux programs them
(`mga_state.c:80`, `:103`, `:548`). The depth buffer rows are the colour byte
pitch apart with 2 bytes per Z16 sample: Mesa reads `buf + x*2 + y*pitch` with
`pitch = frontPitch` (`mgaspan.c:113`-`118`, `mga_xmesa.c:746`-`749`) and
X.Org allocates `depthPitch = frontPitch`, `depth_cpp = bitsPerPixel/8`
(`mga_dri.c:583`, `:585`, `:1183`). `PITCH.iy` is in pixels, a multiple of
32 and at most 4096 (page 3-162, `g400.txt:6730`).

Heap: the 2D driver keeps the visible frame buffer (`fb->size`) and one cursor
page at `PAGE_ALIGN_DOWN(min(vram,16 MiB)) − 4096` when the second CRTC is
off (`driver.c`, `start_with_io`). The 3D heap is the largest run of free
4 KiB pages between `align4096(fb->size)` and the cursor page (or the VRAM
top without a cursor), filtered through the registry inventory exactly as the
Savage owner does, claimed as one `RES_MMIO` extent `matrox-3d` and mapped
once. Inside the heap every resource has a 4 KiB guard page before and after
it (`reserved = padded + 8192`), filled with `0xA55AC33C` and verified after
every job. Padding:

| Kind | Hardware pitch | Padded bytes |
| --- | --- | --- |
| `R3D_COLOR`, `R3D_LINEAR_COLOR` | `((w+31)&~31)·4` | pitch × `((h+15)&~15)` rounded to 4 KiB |
| `R3D_D16` | same byte pitch as the colour target of the same width | same |
| `R3D_TEXTURE` | `w·4` per level | Σ max(32, w·h·4 >> 2k) over the addressable chain, 4 KiB rounded |

Textures live in a separate pool at the top of the heap (`texture_pool_bytes`,
minimum 256 KiB), colour/depth targets below it, as in the Savage service.
The required heap for a mode is two colour targets + one depth + the texture
minimum + guards + 24 KiB of linear scratch; a smaller heap refuses before any
claim.

State saved and restored around every job: the registers the 2D port
initializes once (`PITCH`, `YDSTORG`, `PLNWT`, `SRCORG`, `DSTORG`,
`OPMODE`, `CXBNDRY`, `YTOP`, `YBOT`, `MACCESS`; `engine.c`
`matrox_engine_init`) plus the 3D-only registers with documented reset values
(`ALPHACTRL` = 1, `TEXCTL2` = 0, `ZORG` = 0, `DWGCTL` = 0). All drawing
registers are write-only (every page says "WO"), so restoration cannot be
read back; the backend re-writes the 2D initialization set from the engine's
own fields, the 2D engine re-writes `MACCESS`/`PITCH` before each of its
operations anyway, and the host model verifies the restore trace.

## 5. Validation and admission

`validate` runs before any write, on the copied request, and refuses with
`-EINVAL` (malformed) or `-EOPNOTSUPP` (software fallback, whole context for
`R3D_AUTO` as the contract requires):

- heap and extent checks identical to the Savage (`extent()`), including guard
  reservation, 4 KiB alignment, pool separation and overlap of the five refs;
- depth function `NEVER`, textures below 8×8, A8 with DECAL, G200 with ADD,
  G200 with two stages, non-Q4 vertices, reciprocal-W ratio above 16 per
  triangle, mip draws whose LOD bound exceeds the addressable chain, and mip
  draws on textures below 16×16 → `-EOPNOTSUPP`;
- the mip LOD bound uses planes A = u·rhw and B = rhw: the numerator
  of ∂(A/B) is affine, so its absolute extrema are at vertices. The
  implementation keeps cancellation between (∇A)B and A(∇B), adds a
  derivative-rounding bound, and divides conservatively by Bmin². The sum
  of the axis bounds times texture size must not exceed 2^(levels_hw − 1);
- triangles with a non-flat top covering zero sampled rows → `-EOPNOTSUPP`
  (exact coverage cannot be preserved by rounding the shared-edge split);
- before full-size qualification: 64×64 targets, one triangle; textures
  ≤ 64; blend factors limited to the four alpha selectors (as the Savage) until
  the precision probe passes;
- every triangle is set up (`matrox3d_setup`) before mip derivative arithmetic,
  so malformed RHW/UV and setup overflow (18-bit edge deltas, texture
  coordinates/gradients) refuse before any signed product or emission.

## 6. Boot qualification

Entered only with the run-time token `3d=probe`, after the 2D activation,
with a durable attempt marker `3d-matrox` keyed to the PCI function
(`display_attempt_marker`), the presenter pinned (`fbdev_render3d_prepare`)
and the heap claimed and mapped. The same orchestration as the Savage
(`savage3d_owner.c`): the lead adapts the hook when f7-13 lands.

1. **Identity/preflight**: PCI 102b:{0520,0521,0525,2527}, BAR0 = LFB,
   active 2D engine, heap size, host memory; no register write.
2. **Minimum probe** (64×64, per setup encoding, inside the claimed heap):
   textured Gouraud triangle with depth write and copy; a farther triangle
   failing LESS; a three-colour Gouraud triangle (channel check at
   (16,16) = 34/23/41 ± 2); copy equality; guards; restore. Pass → the
   encoding is latched and the private device's `qualified` latch enables
   the backend through the existing generic service qualification check.
3. **Full-size probe**: the Savage's four scenes (clear + two-stage quad,
   farther red quad, blended mip/trilinear quad, 256 small triangles) at the
   display size, compared row-sampled against the scalar reference (±2
   colour, ±1 depth, exact coverage/copy, hashes logged).
4. **Precision probe**: the Savage's 23 phases (ramps, checkers, mip scenes,
   filter-level sweep with ≥192 levels per unit, 256/128 textures in both
   units, sampled alpha, ten blend pairs, ±16 CLAMP coordinates) through the
   family envelope (`matrox3d_precision.h`, derived from the original
   Savage envelope; integration 6 keeps its newer Savage header separately).
   Pass → `PROJECTIVE_BILINEAR`, 256-square
   textures and all ten blend factors are advertised.

Refusals: a numeric mismatch keeps the lower capability level (minimum or
affine), engine/guard/restore faults quarantine the shared 2D/3D owner for
the boot and retain the marker. On the G200 the full-size/precision phases
run with a single stage (the second sampler is admitted only on the G400).
Its mip phases use a one-unit UV span rather than two, inside its documented
five-origin chain; the 256/128 textures, 4:1 W variation, filtering, alpha and
blend cases are retained. Both units on G400 and the G200 unit measure all
256 filter output levels in the host model.

## 7. Quake III's state set

From the f7-11 census and `tr_shade.c`: two stages (base texture MODULATE +
lightmap MODULATE), blend pairs ONE/ONE, SRC_ALPHA/INV_SRC_ALPHA, ONE/
INV_SRC_ALPHA, DST_COLOR/ZERO, ZERO/SRC_COLOR, ONE/ZERO, alpha test GEQUAL/
GREATER on sampled alpha, LINEAR_MIPMAP_LINEAR/NEAREST on 256/128/64
textures, REPEAT and CLAMP_TO_EDGE, depth LEQUAL with writes, D16, XRGB
targets. Each state still passes the family validator after precision qualification;
qualification alone does not admit geometry beyond the mip-chain bound.
Remaining in software: depth `NEVER`, textures below 8 texels, mip draws
whose LOD bound reaches below the 8×8 level (skinny far geometry), triangles
with reciprocal-W ratio above 16, non-Q4 positions or a non-flat top with no
sampled row, A8+DECAL, and on the
G200 every two-stage pass and ADD environment (Quake III then renders the
lightmap as a second pass with DST_COLOR/ZERO, which the G200 path accepts).
Fog is not part of the contract (software GL).

The complete H1 replay reaches that mip refusal at both 640×480 and
1024×768. GL then pins the entire context to software: 3000 replay triangles
per single-pass frame, 4600 per multipass frame. The first frame has two
GPU sky triangles before the world batch refusal and one hardware/software transition; these are not
accelerated world/entity draws. Later
frames have zero GPU triangles. Generic encoder eligibility remains 100%,
which is not family admission. Thus the Savage 95% world/entity target is
**not met** with this conservative family subset; no GPU performance claim
is made. Mesa `mga_texstate.c:122-147` permits deeper mip chains down to its
32-byte stopping condition, whereas G400 `g400.txt:9318-9321` explicitly
requires at least eight texels including mip maps. This implementation keeps
the vendor bound. The lead must decide whether to commission a separate,
probe-qualified Mesa-compatible deeper-mip path; it is not enabled here.

## 8. Capabilities

Minimum: `max_size 64`, `max_vertices 3`, `texture_units` 2 (G400) / 1
(G200), `max_texture_size 64`, `depth_functions 253` (all but NEVER),
`alpha_functions 255`, `blend_factors 15`, `combine_modes 15` (G400) / 11
(G200: MODULATE, REPLACE, DECAL), `filters 31`, `texture_formats 63`,
`max_pixel_work 4096`. Full-size: `max_size/max_width/max_height` = mode,
`max_vertices 768`, `max_pixel_work 4·w·h`, `AFFINE_EXACT`. Precision:
`PROJECTIVE_BILINEAR`, `max_texture_size 256`, `blend_factors 1023`.
opengl32 and native GL read these through the existing generic
`render3d_backend_caps`; no family name reaches generic code.

## 9. Risks (decided empirically by the probe, never assumed)

1. **Sub-pixel interpolant scaling** (`us`): two setup encodings are tried in
   order; both failing leaves the family in software. Host tests exercise the
   fallback chain with a model switch.
2. **Edge rounding and fill rule** of the sub-pixel engine: exact coverage is
   required; a mismatch refuses full-size qualification (minimum coverage is
   integer-aligned and passes or fails cleanly).
3. **Mip filter mnemonics** (MM2S/MM4S): decided by the BILINEAR_MIP and
   TRILINEAR precision phases.
4. **Modulation rounding** (9.15 factors, "saturate, rup"), texel expansion,
   blend rounding: covered by the ±2 envelope and the precision phases.
5. **Z row pitch** equals the colour byte pitch (Mesa/X.Org evidence); a
   wrong pitch shows as depth corruption in the minimum probe's far-triangle
   test and in the guard pages of the next resource.
6. **G450/G550** are programmed as G400 following Linux/Mesa; the probe is
   per boot and per chip.
7. **Write-only registers**: D-F2 applies. The driver is the only 3D-state
   writer. It re-emits its shadowed baseline on every safe exit, then every
   probe proves a guarded positive 2D solid fill and restores again. Failure
   quarantines. A hung FIFO retains claims/marker rather than accepting
   unsafe restoration writes.
8. **FIFO depth** of G450/G550 is modelled as 16 like the G400.

## 10. Hardware qualification procedure (owner with a G-series card)

1. Boot the canonical image with the run-time token `3d=probe` added to the
   kernel options; the 2D port must report `[accel] driver=matrox … ready=1`.
2. Read the serial log lines `[3d] probe phase=identity`, `phase=preflight`,
   `phase=armed`, `[3d] fullsize result=…`, `[3d] precision result=…` and
   `[3d] probe result=qualified|blocked|failed reason=…` with phase, expected/actual,
   guards and write/triangle/copy counters. Setup encoding is latched internally.
3. A `blocked` result with `reason=engine_probe` and both encodings failing is
   risk 1/2; the desktop must still work (software 3D). A `failed` result
   quarantines 2D and 3D for the boot and leaves the marker: reboot without
   the token, then send the log.
4. Require both full-size and precision log results to be `qualified` before
   expecting full-mode admission; minimum-only qualification still caps
   targets at 64×64. Record the canonical image SHA-256 and serial log.
   Run Quake III with `r_depthbits 16`, `r_stencilbits 0`,
   `r_texturebits 32` and `CIUKI_GL_CENSUS=60` for the census lines, then the
   Graphics Demo cube and textured scene (contract T2).
5. Successful and safely refused attempts clear their marker. A retained
   failed-attempt marker blocks the next probe by design; use the ordinary
   display retry workflow after reviewing the log, never delete a live
   reservation or force hardware admission.

## 11. Implementer corrections after source audit (2026-10-28)

The interrupted draft is retained, with these corrections before integration:
DR4/8/12 carry **red/green/blue**, respectively (G400 `g400.txt:5374`),
not packed-byte order. Every clear establishes MACCESS, masks and clipping
without depending on a preceding draw. TEXFILTER enables filtered alpha
(bit 20, `g400.txt:8930`; Mesa `mgatex.c:323`). Blend factors unavailable
in a selector position (SRC_COLOR as source, DST_COLOR as destination and
their inverses) refuse before emission (`g400.txt:2808-2838`).

D-F2 applies: this driver alone owns the write-only 3D registers. Restore
means the shadowed idle baseline, re-emitted after completion and numeric
refusal. Each probe also executes a positive 2D solid fill into a guarded
claimed target and restores that baseline; failure quarantines. A hung FIFO
cannot safely accept restore writes: retain its claim/marker and quarantine.
D-F5 makes WARP firmware available, but this documented host-setup path
does not execute WARP and needs no firmware packaging. No DMA is enabled.
The current checkout has no f7-13 background orchestration; use the current
record hook, matching Savage, for the lead to adapt on integration.

The Windows 98 Resource Kit `wrkc12_4.htm:163-164` includes hardware-assisted
DirectDraw/Direct3D acceleration among the Entertainment PC recommendations. It supplies
no Matrox register recipe; only the vendor specification and pinned upstream
code below determine programming. No private driver-pack material is copied.

## 12. Individual cited source hashes

Verified against the lead's read-only cache; paths below are relative to
`/home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/`.

| File | SHA-256 |
| --- | --- |
| `matrox-specs/mga-g200_spec.pdf` | `c851312b53443dfa8c99d1703ee005f56eb8cec20c86fded9fee0da1518c2fe1` |
| `matrox-specs/g200.txt` | `db2b0c0d54e2ac167c6132d1113250ea6d06f4a14e1b35a22625144538c97471` |
| `matrox-specs/mga-g400_spec.pdf` | `8722dd6d9e2353e1c2ba39fab1962e69058e665221a9fb89ef1818308c9b82cb` |
| `matrox-specs/g400.txt` | `c1c0b1328475db1f7bccf2f2f4306d877be24f3e385e80b968f4caf53a5d724b` |
| `linux-6.1-mga/mga_drv.h` | `18434839f7530f11e529a629ba4bb06d32883300d9245a55f6e55a2a83b16bdb` |
| `linux-6.1-mga/mga_state.c` | `d73187226a7b30799b4c750cf2c9ed4781b207a7dd908d4faae2910ac65c6176` |
| `linux-6.1-mga/mga_dma.c` | `a28a9a6ebc209e8ccf5c296238626483f33df2df277b9dad415712e09fb76616` |
| `linux-6.1-mga/mga_warp.c` | `75a787c02080999754c79917b7f4dca119db501d3f8ab7f7780af0947ce5fa2a` |
| `linux-6.1-mga/mga_drm.h` | `8af9981e1cbabbf1f2ff69db1bbec9da874cbd9a8148b6136705d6f698ad81d6` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mgaregs.h` | `13b593a6eaf18dc9282eb23f06d21a51f4b4eb32fb8c5bcbc913fe4d9014bb13` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mga_texstate.c` | `af89c0a41b83ed733994eb04822d969c08c66d236dd2d48b08ece96af8ad167e` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mgastate.c` | `e9ea4adf485494175f10ed546e873a6b46b06f630dc60cd0b987fa18abfb92a0` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mgatex.c` | `1526cc2a87779a22f4b090a81e4e62824551b3f737fe6b1d3d04096b45d0c673` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mgatexmem.c` | `5fa9408016fb0b51801e10e60996cd87dc836e840c35ba20c4d05f7a853550d2` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mgaspan.c` | `af2163d0c42552e0d089dea085372bb0dbfc8581ac5d9b1cd8d2f9b7aa5e66fd` |
| `mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/mga/mga_xmesa.c` | `bff66a8306ec016decf5fb16ba85f5707ec08b7ba42bd11aed02080240afc1ad` |
| `xf86-video-mga/xf86-video-mga-2.0.0/src/mga_dri.c` | `5ffd3dcccb89f01c74084eb195ef8fb2a4b09847e0687f822453e2a9f6ee4871` |
| Resource Kit `build/rskt/wrkc12_4.htm` (read-only) | `01e401c4f8e0926f3ee262180003522c5f72a34c3093ffa23c067608f3c61b19` |

## 13. Final host evidence and integration boundary

The source audit also keeps early alpha testing disabled, validates bounded
RHW/UV and gradients before mip arithmetic, refuses imprecise zero-row split
setups, and validates scanout/cursor endpoints before heap arithmetic.
The Matrox precision envelope retains its arithmetic and skips disabled
texture stages in two loops, allowing the single-unit family to use it
without dereferencing a nonexistent sampler. Two-stage semantics remain
unchanged. The independent interpreter decodes G200 TEXCTL/ALPHACTRL, G400
TDUALSTAGE, broadcast/map-1 texture addressing and both setup encodings.
The test programme includes 10,000 malformed requests, 512 subpixel traces
(496 admitted, 16 clean refusals), combined guard corruption plus deadline
overrun, positive/refused boot paths, exclusive consumer attachment,
shutdown/revocation, real service fences and six textures evicted/reuploaded
through a three-slot pool. No public ABI or SDK header changes.

Primary upstream entry points: [Mesa 7.11.2 archive](https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2),
[Linux v6.1 mga](https://github.com/torvalds/linux/tree/v6.1/drivers/gpu/drm/mga),
[X.Org 2.0.0 archive](https://www.x.org/releases/individual/driver/xf86-video-mga-2.0.0.tar.gz).
The individual source hashes above are the provenance for source-level
research; no source archive or driver-pack material is vendored.

The current tree has no f7-13 background probe hook. This implementation
uses the existing post-activation record hook, as authorized by f7-25; the
lead must adapt it if f7-13 is present on the integration branch. Hardware
qualification and the canonical image remain the lead's work. Full exact
commands/results, sanitizer environment limits and H1 counts are recorded in
`build/codex/f7-25-report.md`. Host/model results do not qualify silicon.


## Integration 6 merge boundary

Earlier references to a checkout without f7-13 describe the premerge family
branch. Integration 6 contains Savage's background orchestration. Matrox
retains its existing opt-in synchronous hook; this merge does not transplant
Savage lifecycle behavior into another family or qualify Matrox mode switches.

Savage's precision header and oracle fixture remain byte-for-byte equal to
integration 6. The Matrox precision envelope
(`matrox/matrox3d_precision.h`) stays separate so its G200 single-stage checks do not alter Savage qualification. The shared contract
and default GL build retain integration 6 behavior. `GL_KERNEL_FAMILY_MATROX`
selects the Matrox H1 fixture without the T23-specific full-screen lease and
mode-switch replay; these Savage hardware paths are not Matrox evidence.
The normal production GL/provider/wire workload and Matrox fallback counts
remain exercised at both H1 sizes.

Integration 6 enforces `KHEAP_MAX_REQUEST` at compile time. Matrox probe
services, resource shadows, rings and mip staging therefore use the existing
`desktop_buffer_alloc(..., MM_IO_REQUEST)` page-backed interface, following
Savage's owner allocation contract. Page-rounded charges, allocation prefixes
and the family resident bytes share the existing `R3D_RAM_LIMIT`; refusal
returns `-ENOMEM`, with the existing cleanup and quarantine behavior. No
heap limit, physical memory limit or public interface is changed. The owner
fixture enforces the small-heap limit separately and checks page allocation
sizes, charges and matching frees.
