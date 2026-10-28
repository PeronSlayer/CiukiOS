# ATI Radeon R100/R200 hardware 3D backend (f7-24)

Written 2026-10-26 (worktree `wt/f7-24`, base `main` be7852468) for the
[f7-24 directive](../directives/f7-24-ati-radeon.md). This family cannot be
tested on hardware here and QEMU emulates none of these 3D engines: every
statement below is a source-backed design that is host-tested against a fake
engine, probe-gated at boot and inert until the boot qualification passes on a
real chip. The backend mirrors the Savage one
([f7-3d-acceleration-contract.md](f7-3d-acceleration-contract.md),
[savage-fullsize-admission.md](savage-fullsize-admission.md)): same service,
same admission predicate, same precision policy, same orchestration.

## 1. Sources (pinned, read-only, never vendored)

Local cache `build/downloads/f7-sources/` (main tree). Lead-provided downloads (no new download was needed in this sandbox), with
SHA-256:

| File | Origin | SHA-256 |
| --- | --- | --- |
| `linux-6.1-radeon/radeon_reg.h` | [torvalds/linux v6.1 drivers/gpu/drm/radeon](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/radeon/radeon_reg.h) | `ed549525a1668899fd0c348ac6d1f52f3d9f4abc08d10387c8666cceaa5c552d` |
| `linux-6.1-radeon/r100.c` | same tree, `r100.c` | `5e3bdf7a84a086505dc59215467c26ab97eb000b29a46a32856a125c496b600d` |
| `linux-6.1-radeon/r100d.h` | same tree | `1d0c10bec76065794ce6daf753d51ddc70d79a86a2ea52417963b0dea258866a` |
| `linux-6.1-radeon/r200.c` | same tree | `7bb47ef0c76037c8b9930d002cb3e265f77580aa937ddd5a95786d42a4e0e5a9` |
| `linux-6.1-radeon/r100_track.h` | same tree | `1814fde029725431a85313352a49ab623db80293f5b834d43f6e7c39ade4c3d5` |
| `linux-6.1-radeon/radeon.h` | same tree | `f9ce62554fa026a135747808374ebf473cd1490ce020748a96db6b9bb4a39135` |
| `linux-6.1-radeon/radeon_asic.h` | same tree | `f408e19d79c7c125088575e3ac35691c23b3c2c882edbe83ad970846496ef596` |
| `linux-6.1-radeon/radeon_drm.h` | `include/uapi/drm/radeon_drm.h`, v6.1 | `4d1d189cab7ab4cf256d011f62ec43985fadcc16a995e0acb84e70b3ac8ff52f` |
| `xf86-video-ati/xf86-video-ati-6.14.6.tar.bz2` | [x.org release archive](https://www.x.org/archive/individual/driver/xf86-video-ati-6.14.6.tar.bz2) | `aa5286b3e4f0187d7df14785c06dd800255d9405205dbf061da5d77df86bec36` |

Already pinned by f7-02: `MesaLib-7.11.2.tar.bz2`
(`57746aa8c60bc6f5596ec6c6c7274de40fa12edce5ec33fe899e704fa00b5357`), the
last Mesa with the classic `radeon`/`r200` DRI drivers. The driver pack under
`third_party/` contains no ATI register documentation (checked 2026-10-26:
only storage/ASPI `.sys` files). No ATI register manual was found in the supplied driver pack; programming
uses the pinned upstream implementations below. This is not a claim that no
vendor publication exists elsewhere.

Abbreviations: `L` = `linux-6.1-radeon/`, `M` =
`mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/`, `X` =
`xf86-video-ati/xf86-video-ati-6.14.6/src/`. The in-tree 2D driver is
`src/kernel/drivers/video/radeon2d/`.

## 2. Chips and PCI IDs

The 2D driver's table (`radeon2d/driver.c:70-156`, `RADEON_CHIPS`) groups the
matched devices into `RADEON_R100`, `RADEON_R200`, `RADEON_R300`, `RADEON_R4XX`
(`engine.h`, `enum radeon_group`). The 3D backend serves only the first two groups; R300/R4xx
nominations stay software (their 3D engine is a different design: Linux
`r300.c`, X.Org `R300PrepareComposite`).

| Group | PCI 1002:xxxx | Marketing | TCL |
| --- | --- | --- | --- |
| R100 | 5144 5145 5146 5147 | Radeon 7200 (R100 QD/QE/QF/QG) | yes |
| R100 | 5159 515a | Radeon VE / 7000 (RV100 QY/QZ) | no |
| R100 | 4c59 4c5a | Mobility M6 (RV100 LY/LZ) | no |
| R100 | 5157 5158 | Radeon 7500 (RV200 QW/QX) | yes |
| R100 | 4c57 4c58 | Mobility M7 (RV200 LW/LX) | yes |
| R200 | 4242 4243 5148 514c 514d | Radeon 8500 / 9100 / AIW 8500 (R200 BB/BC/QH/QL/QM) | yes |
| R200 | 4966 4967 | Radeon 9000 (RV250 If/Ig) | yes |
| R200 | 4c64 4c65 4c66 4c67 | Mobility M9 (RV250 Ld/Le/Lf/Lg) | yes |
| R200 | 5960 5961 5962 5964 | Radeon 9200 (RV280) | yes |
| R200 | 5c61 5c63 | Mobility 9200 (RV280) | yes |

TCL presence follows `M/radeon/radeon_screen.c:593-627` (RV100 LY/LZ/QY/QZ get
no `RADEON_CHIPSET_TCL`) and X.Org `X/radeon_commonfuncs.c` R100 branch (only
`CHIP_FAMILY_RADEON` and `RV200` write `SE_CNTL_STATUS=0`, the rest
`RADEON_TCL_BYPASS`). The backend never uses TCL; the bypass bit is set only
for the four RV100 IDs, exactly as X.Org does.

## 3. The 3D engine as used here

Everything is the classic fixed-function pipeline of the R100 ("Rage 6")
and R200 ("Rage 8") families, driven through the register FIFO.

- **Triangle setup**: vertices already in window coordinates, with reciprocal
  W, written to `SE_PORT_DATA0` (`L/radeon_reg.h:2730`) after `SE_VF_CNTL`
  (`:2703-2729`: `VF_PRIM_TYPE_TRIANGLE_LIST=4`, `VF_PRIM_WALK_DATA=3<<4`,
  `VF_RADEON_MODE=1<<8`, vertex count at bit 16). The X.Org MMIO composite
  path uses this exact mechanism (`X/radeon_exa_render.c:2455-2473`, vertex
  floats at `:2268-2275`).
- **Vertex format**: R100 `SE_VTX_FMT` (`:2678-2702`) = `Z|W0|PKCOLOR|ST0[|ST1]`,
  dword order X,Y,Z,W0,colour,S0,T0,S1,T1 (Mesa's swtcl attribute order,
  `M/radeon/radeon_swtcl.c:116-124,131-134,180-191`). R200
  `SE_VTX_FMT_0` (`:3228-3255`) = `Z0|W0|(PK_RGBA<<11)`, `SE_VTX_FMT_1`
  (`:3256-3262`) = 2 components per used texture; same dword order
  (`M/r200/r200_swtcl.c:105-120`).
- **Coordinate conventions** (the central decision, see §4): R100
  `SE_COORD_FMT` (`:2465-2479`) = `VTX_XY_PRE_MULT_1_OVER_W0 |
  VTX_Z_PRE_MULT_1_OVER_W0 | TEX1_W_ROUTING_USE_W0`, i.e. X, Y, Z are already
  divided, W0 is 1/w and S/T are raw; this is Mesa's `needproj` branch
  (`M/radeon/radeon_swtcl.c:283-290`) with the texture coordinates left to
  the chip. R200: `SE_VTE_CNTL` (`:2742-2753`) = `VTX_XY_FMT | VTX_Z_FMT`
  (W0 is 1/w since `VTX_W0_FMT` is clear), no viewport enables
  (`M/r200/r200_swtcl.c:257-262`, clear-quad precedent `:800-812`);
  `SE_VAP_CNTL` = `VF_MAX_VTX_NUM` without `FORCE_W_TO_ONE`
  (`:2732-2741`; X.Org `R200_SE_VAP_CNTL` at `X/radeon_commonfuncs.c:789`).
- **Rasterizer setup**: `SE_CNTL` (`:2401-2446`) = `BFACE_SOLID | FFACE_SOLID
  | DIFFUSE_SHADE_GOURAUD | ALPHA_SHADE_GOURAUD | VTX_PIX_CENTER_OGL |
  ROUND_MODE_ROUND | ROUND_PREC_16TH_PIX` and no viewport transform (X.Org
  `X/radeon_commonfuncs.c` R100/R200 branches; the 16th-pixel precision keeps
  the qualified 1/16-pixel coordinates exact). Scissor: `PP_CNTL.SCISSOR_ENABLE`
  with `RE_TOP_LEFT=0`, `RE_WIDTH_HEIGHT=(w)|(h<<16)` (`:2391-2396`, X.Org
  `radeon_exa_render.c:743-745`; Linux reads `maxy` from it `L/r100.c:1700`).
  R200 additionally `RE_CNTL = PERSPECTIVE_ENABLE | SCISSOR_ENABLE`
  (`:2447-2458`, Mesa `r200_state_init.c` "SET_RE_CNTL"), `RE_AUX_SCISSOR_CNTL=0`,
  `RB3D_DEPTHXY_OFFSET=0`, `SE_VTX_STATE_CNTL=0`, `PP_CNTL_X=0`,
  `PP_TXMULTI_CTL_n=0` (X.Org `radeon_commonfuncs.c:777-800`).
- **Colour buffer**: `RB3D_CNTL` colour format 6 = ARGB8888 at bit 10
  (`:2254-2278`), `RB3D_COLOROFFSET` 16-byte aligned (`:2279-2280`),
  `RB3D_COLORPITCH` in pixels, mask `0x1ff8` (`:2281-2282`; X.Org refuses pitches
  that are not multiples of 8 pixels, `radeon_exa_render.c:654`).
- **Depth**: `RB3D_ZSTENCILCNTL` (`:2320-2371`) = `DEPTH_FORMAT_16BIT_INT_Z |
  Z_TEST_x | STENCIL_TEST_ALWAYS | KEEP*3 | Z_WRITE_ENABLE` (Mesa initial image
  `M/radeon/radeon_state_init.c:918-924`, `radeonDepthFunc/DepthMask`
  `radeon_state.c:260-304`); `RB3D_CNTL.Z_ENABLE` (bit 8) when testing or
  writing; `RB3D_DEPTHOFFSET`/`DEPTHPITCH` (`:2288-2293`, pitch mask `0x1ff8`).
  Linux derives 2 bytes per depth pixel from format 0 (`L/r100.c:1758-1761`).
  The same register family also exposes D24 plus stencil comparison/operations
  (`L/radeon_reg.h:2320-2371`); this backend deliberately qualifies only D16
  and keeps stencil disabled. Hardware TCL is bypassed or unused; transformation,
  lighting, clipping and unsupported fog/offset remain consumer software work.
- **Blending**: `RB3D_BLENDCNTL` (`:2225-2253`) = `COMB_FCN_ADD_CLAMP |
  src<<16 | dst<<24` with the GL factor codes 32..41 (Mesa
  `radeon_state.c:148-258`), enabled by `RB3D_CNTL.ALPHA_BLEND_ENABLE`.
- **Alpha test**: `PP_MISC` (`:1905-1927`) = ref | func<<8, enabled by
  `PP_CNTL.ALPHA_TEST_ENABLE` (Mesa `radeon_state.c:65-107`).
- **Textures**: two of the three (R100) / six (R200) units.
  R100: `PP_TXFILTER_n` (`:1931-1983`), `PP_TXFORMAT_n` (`:1984-2030`),
  `PP_TXOFFSET_n` (`:2051-2062`, 32-byte aligned), `PP_TEX_SIZE_n` /
  `PP_TEX_PITCH_n` (`:2082-2096`), combiners `PP_TXCBLEND_n` (`:2098-2181`) and
  `PP_TXABLEND_n` (`:2182-2220`), unit stride 0x18. R200: `PP_TXFILTER_n`
  (`:2758-2812`, stride 0x20), `PP_TXFORMAT_n` (`:2813-2863`), `PP_TXFORMAT_X_n`
  (`M/r200/r200_reg.h:924-955`), `PP_TXSIZE_n`, `PP_TXPITCH_n`, `PP_TXOFFSET_n`
  (`:2892-2909`, stride 0x18), combiners `PP_TXCBLEND_n`/`TXCBLEND2_n`/
  `TXABLEND_n`/`TXABLEND2_n` (`:2949-3227`, stride 0x10, `r200_reg.h:1416-1419`).
  Formats used: ARGB8888 (6, `ALPHA_IN_MAP`), RGB565 (4), ARGB4444 (5,
  `ALPHA_IN_MAP`) (`M/radeon/radeon_texstate.c:85-109`, R200 `tx_table_le`
  `r200_texstate.c:111-118`). L8 and A8 are uploaded expanded to ARGB8888
  (the canonical shadow already holds alpha 0xff / RGB 0xff): Mesa notes the
  R100 I8 path "doesn't work for luminance textures" (`radeon_texstate.c:288-292`).
  Power-of-two sizes only (`TXFORMAT_WIDTH/HEIGHT` log2 fields); hardware
  maximum 2048 (`M/radeon/radeon_context.c:293`,
  `M/r200/r200_context.c:371`, twelve mip levels), qualified maximum 256 (§7).
- **Texture memory layout** (Mesa miptree, `M/radeon/radeon_mipmap_tree.c:99-118,127-183`;
  row alignment 32 bytes for R100/R200, `radeon_common_context.c:253-256`):
  level i row stride = `max(32, w_i*bpp)` rounded up to 32, level size =
  stride * h_i, levels consecutive from the 32-byte aligned base, total
  rounded to 32. `PP_TXOFFSET` points at level 0; `PP_TEX_PITCH` carries
  stride-32 (`radeon_texstate.c:1092-1097`, X.Org `:450-456`). `MAX_MIP_LEVEL`
  = levels-1 (`:1056-1057`).
- **Filters and wrap** (`M/radeon/radeon_tex.c:58-130` wrap, `:170-220` filter):
  `CLAMP_S/T_WRAP` for repeat, `CLAMP_S/T_CLAMP_LAST` for clamp-to-edge,
  `BORDER_MODE_OGL`; MAG `LINEAR`/`NEAREST`; MIN `NEAREST`, `LINEAR`,
  `NEAREST_MIP_NEAREST`, `LINEAR_MIP_NEAREST`, `LINEAR_MIP_LINEAR`. R200
  identical codes (`r200_tex.c:63-140,216-285`), plus `TXFORMAT_X =
  TEXCOORD_NONPROJ | LOD_BIAS_CORRECTION` (`r200_state_init.c` texture init,
  `r200_reg.h:955`).
- **Combiners** (per stage; `CURRENT` at stage 0 is the diffuse colour on R100,
  Mesa `radeon_texstate.c:169-178,404-470`; R200 uses `DIFFUSE` at slot 0 and
  the previous stage's output register otherwise, `r200_texstate.c:405-430`):

  | Native mode | R100 colour (`TXCBLEND`) | R100 alpha (`TXABLEND`) |
  | --- | --- | --- |
  | MODULATE | A=CURRENT, B=Tn, C=ZERO, ADD | A=CURRENT, B=Tn, C=ZERO, ADD |
  | REPLACE | A=B=ZERO, C=Tn (alpha-only texture: C=CURRENT) | C=Tn (RGB-only texture: C=CURRENT) |
  | ADD | A=CURRENT, B=ZERO+COMP (=1), C=Tn (alpha-only: C=ZERO) | A=CURRENT, B=Tn, C=ZERO |
  | DECAL | BLEND: B=Tn colour, A=CURRENT, C=Tn alpha (RGB-only: REPLACE colour) | C=CURRENT |

  These are the Mesa slot assignments for GL_MODULATE/REPLACE/ADD and
  GL_INTERPOLATE (`radeon_texstate.c:405-448`), applied to the native
  per-format semantics of `render3d_scalar.c:combine`. R200 uses the same
  slots with `TXC_OP_MADD`/`TXC_OP_LERP` (`r200_texstate.c:538-575,634-670`),
  texture n in register Rn, output register R0 for every stage
  (`r200_texstate.c:905-935`: the last stage always gets register 0; a single
  stage or two stages both write R0 and stage 1 reads R0 as "previous"),
  `TXCBLEND2/TXABLEND2 = CLAMP_0_1 | OUTPUT_REG_R0` (X.Org `:1118-1122`).
  Scale 1x, `CLAMP_TX` on R100.
- **Fog, stencil, polygon offset, dithering**: not used (fog disabled in
  `PP_CNTL`, stencil ALWAYS/KEEP, `ZBIAS` off, `DITHER` off, `ROUND_ENABLE` off,
  `PLANE_MASK_ENABLE` with `RB3D_PLANEMASK=0xffffffff`, `RB3D_ROPCNTL=ROP_COPY`).
  Fog and polygon offset remain software, as for the Savage.
- **Clears and the conversion copy**: the 2D engine, as the 2D driver already
  drives it (`radeon2d/engine.c:fill_rect/copy_rect`), but with
  `DST_PITCH_OFFSET`/`SRC_PITCH_OFFSET` pointing at the owned resource
  (`(pitch/64)<<22 | offset>>10`, X.Org `X/radeon_exa.c:149-165`,
  `Emit2DState` `X/radeon_exa_funcs.c:90-131`). Explicit selectors in
  `DP_GUI_MASTER_CNTL` are mandatory: SRC bit 0, DST bit 1
  (`L/radeon_reg.h:749-750`); solid uses DST and copy uses both, with
  `GMC_SRC_DATATYPE_COLOR` (`L/radeon_reg.h:783`,
  `X/radeon_exa_funcs.c:200-206,255-263`). Otherwise the chip uses
  `DEFAULT_PITCH_OFFSET`, which belongs to scanout. The fake engine models
  this default and rejects any resulting access outside the claim.
  Colour clears use datatype 6,
  depth clears datatype 4 (16 bpp) with the clear value in
  `DP_BRUSH_FRGD_CLR` (X.Org `RADEONGetDatatypeBpp` `radeon_exa.c`, `PrepareSolid`
  `radeon_exa_funcs.c:160-216`). Both registers are saved and restored with a
  checked readback: the 2D driver programs `DST_PITCH_OFFSET` once at
  activation (`engine.c:radeon_engine_activate`).

## 4. Command submission, idle proof, caches

- **MMIO FIFO only.** The 2D driver forbids the CP (`CP_CSQ_CNTL` must read
  zero, `engine.c:idle_proof`/`before_draw`), so the 3D backend uses the same
  64-entry register FIFO: wait until `RBBM_STATUS & 0x7f >= n` then write n
  registers (X.Org `RADEONWaitForFifoFunction` `X/radeon_accel.c:128-139`,
  `BEGIN_ACCEL(n)`/`OUT_ACCEL_REG` `:589-591`; Linux
  `r100_rbbm_fifo_wait_for_entry` `L/r100.c:2477-2491`). Each triangle is one
  `SE_VF_CNTL` write plus 27 vertex words (R100 and R200 alike), so the
  backend waits for 28 free entries per triangle and never exceeds the FIFO.
  No `WAIT_UNTIL` packet is queued (same rule as the 2D driver): completion is
  proven by polling.
- **Bounded waits and watchdog.** Every FIFO wait polls at most
  `RADEON_POLLS` times with a 5 us delay inside one 20 ms execution window
  (`slice_started`), exactly the Savage rule; CPU transfers are chunked with
  1 ms checks. Expiry returns `-ETIMEDOUT`; the service then quarantines unless
  the engine still proves idle (render3d.c).
- **Idle and cache flush.** `radeon_engine_idle()` (the 2D proof: FIFO empty,
  `GUI_ACTIVE` clear, 2D destination cache flushed) followed by the 3D caches:
  write `RB3D_DSTCACHE_CTLSTAT = RB3D_DC_FLUSH_ALL (0xf)` and
  `RB3D_ZCACHE_CTLSTAT = RB3D_ZC_FLUSH_ALL (0x5)` and poll `RB3D_DC_BUSY`
  clear (X.Org `RADEONEngineFlush` for chips up to RV280
  `X/radeon_accel.c:189-205`; Linux fence emission flushes the same two
  registers before any CPU read, `L/r100.c:868-882`; bits `L/radeon_reg.h:1610-1631`).
  This runs after the clears (2D) before 3D draws, after the draws before the
  copy and the readback, and before every state restoration.
- **Texture state/cache ordering.** Every job re-emits the complete texture
  state, including offsets even when their values are unchanged. Mesa explicitly
  avoids delta-only state imports to prevent stale texture caches
  (`M/radeon/radeon_texstate.c:827-831`, `M/r200/r200_texstate.c:1118-1124`).
  CPU writes are drained before state/vertices; no unproven cache register is
  introduced for eviction/reupload.
- **Fences.** Each job completes synchronously under the hook (idle proof +
  flushed caches + readback into the canonical shadow) like the Savage; the
  service's generation/sequence/ordinal fences remain software.
- **VRAM.** One disjoint heap claimed by the owner glue: it begins above the
  scanout (`align4096(fb->size)`), ends below the 2D driver's cursor page
  (`engine.cursor_offset`), excludes every live registry MMIO reservation
  page-wise (the Savage `available_heap` scan), and must hold two padded
  display-sized colour buffers, one D16, the 256 KiB minimum texture pool,
  guards and the probe scratch. 3D addresses are chip-local:
  `engine.local_base + heap offset` (the 2D driver's `pitch_offset` uses the
  same base; QEMU's R100 model has base 0). All offsets are 4096-aligned
  (register requirements are 16/32 bytes).
- **Resource layout.** Colour: pitch `((w+16)&~15)*4` bytes (a multiple of 64
  for the 2D engine and of 32 for `COLORPITCH`), `h+1` rows; D16: pitch
  `((w+32)&~31)*2` bytes, `h+1` rows; both padded by at least one column and
  one row so that either inclusive or exclusive `RE_WIDTH_HEIGHT` semantics
  stay inside the allocation; 4096-byte guard pages on both sides, verified
  on every job. Linear copy target: canonical `w*4` pitch, admitted for COPY
  only when `w % 16 == 0` (640/800/1024 and the 64-square probe); other widths
  refuse COPY with `-EOPNOTSUPP` (software fallback).
- **State saved and restored** around every job with checked readback: the
  38 (R100) / 55 (R200) 3D registers listed in `radeon3d.c`
  (`r100_state[]`, `r200_state[]`), plus `DST_PITCH_OFFSET`
  and `SRC_PITCH_OFFSET`. `RB3D_CNTL` returns to the zero the 2D activation
  wrote. `DP_GUI_MASTER_CNTL`, `DP_BRUSH_FRGD_CLR`, `DP_WRITE_MSK`, `DP_CNTL`
  are rewritten by every 2D driver operation and are restored without readback
  (`DP_GUI_MASTER_CNTL` reads back masked on QEMU and in the host model).
  A failed restoration quarantines the common owner (render3d fault), like the
  Savage.

### Vertex encoding decision

X, Y: window Q16.16 to IEEE single (exact for multiples of 1/16 pixel), with
`VTX_PIX_CENTER_OGL` (pixel centres at half integers) and 16th-pixel rounding
to nearest. Z: `native_z / 65535` as a correctly rounded rational (the chip's
depth input is a 0..1 float: Mesa's hardware viewport maps NDC to 0..1
`radeon_state.c:1400-1436`; with the viewport transform disabled the vertex
carries that value). W0: `rhw / 65536` (1/w). Colour: packed
`0xAARRGGBB` written as-is with the default BGRA colour order (Mesa selects
`COLOR_ORDER_RGBA` for its R,G,B,A byte order, `radeon_ioctl.c:154`; the
default order therefore expects B,G,R,A bytes, which is the native packing).
S, T: Q16.16 to float; the chip interpolates them with W0. All of these are
expectations that the boot probe verifies (channel order, depth value,
texture quadrant, perspective) before anything is advertised.

## 5. Boot qualification and the precision envelope

Same orchestration as the Savage (`radeon3d_owner.c` mirrors
`savage3d_owner.c`): with the run-time token `3d=probe`, after the 2D driver
activated, the owner glue runs from the driver's `record` hook (worker context
outside the presenter): identity check (PCI 1002 device in the R100/R200
groups, BAR0 = framebuffer lease, registers mapped), heap planning against
the registry inventory, presenter pin (`fbdev_render3d_prepare`), heap claim/mapping and scratch preflight, then a durable
marker `3d-radeon` keyed by vendor/device (a retained marker refuses) before
any engine or VRAM write, then:

1. **Minimum probe** (64x64, one textured Gouraud depth-tested triangle,
   clear, copy, guards, restore): identical scene and checks to the Savage
   probe (interior sample ±2 channel levels, depth 16384 ±1, texture
   quadrant, copy equality, guards, state restored). Success enables the
   64x64/one-triangle admission.
2. **Full-size probe** (display-sized colour/D16, both stages, depth, blend,
   mip/bilinear, 256-triangle lists): hardware versus the scalar reference,
   ±2 channel / ±1 depth over sampled rows, copies and guards exact.
3. **Precision probe**: the 23 phases of `savage-fullsize-admission.md`
   through the family-neutral oracle (`render3d_precision.h`, shared outside
   the Savage directory with its arithmetic unchanged) with the same declared policy:
   delta 1/16 texel, at least 8 filter-fraction bits, LOD error 1/16 octave,
   at least 192 distinct levels on the one-dimensional edge sweep. Failure
   refuses only the projective feature.

Log records keep the Savage shape with the family token: `[3d] probe
phase=... pci=1002:xxxx group=R100|R200`, `[3d] fullsize result=...`,
`[3d] precision result=...`, `[3d] probe result=...`. The marker is cleared
only after a successful run; engine, guard or restore faults quarantine both
domains and retain the marker, claims and mappings.

## 6. Admission and capabilities

The admission predicate is the Savage one (`validate()`): resource extents
inside the claimed heap with guard pages, flags/resources consistency,
clear depth ≤ 65535, vertex ranges, texture sizes ≤ 64 until perspective
qualification then ≤ 256, DECAL of A8 refused, COPY only to a linear buffer
(and only for `w % 16 == 0`), the whole blend selector set only after the
precision probe, varying-W triangles only after perspective qualification
with W ratio ≤ 4, 1/16-pixel coordinates, the mip-gradient and
derivative-error bounds, and every vertex list dry-run before any write.
`query_caps` publishes `max_size`/`max_width`/`max_height` = the qualified
mode, `max_vertices` 768, `max_pixel_work = 4*w*h`, `sampling_features`
`AFFINE_EXACT` and, after the precision probe, `PROJECTIVE_BILINEAR` with
`max_texture_size 256` and all ten blend selectors. opengl32 and the native
GL consumer read these through the existing family-neutral capability
snapshot; nothing in `sdk/` names the family.

Whole-context fallback (contract 7.2) stays: a refusal before the first
submission of a frame moves the context to software for good.

## 7. Quake III's state set

Base textures 128..256 square mipmapped bilinear/trilinear REPEAT, 128
lightmaps CLAMP, two stages MODULATE, depth LEQUAL with and without write,
blend pairs ONE/ONE, SRC_ALPHA/INV_SRC_ALPHA, ONE/INV_SRC_ALPHA,
DST_COLOR/ZERO, ZERO/SRC_COLOR, ONE/ZERO, alpha test GE, flat/Gouraud: all
expressible with the registers above and admitted after the full-size and
precision probes. Stays in software: fog, polygon offset (depth-plane
translation when in D16 range, as on the Savage), stencil, textures above
256, independent per-stage Q, D24/S8 pixel formats (the provider predicate
already refuses them; a Quake III trial must select `r_depthbits 16`,
`r_stencilbits 0`).

## 8. Risks

1. **Register semantics not confirmed on silicon**: the W0/pre-multiplied
   combination (§4), the Z scale (65535 vs 65536), the colour byte order,
   `RE_WIDTH_HEIGHT` inclusive/exclusive, the FIFO entry cost of
   `SE_PORT_DATA0` writes, the 2D-engine depth clear at 16 bpp. Each is
   checked by the minimum probe or contained by padding; a mismatch refuses
   admission and nothing is advertised.
2. **RV100/M6 lack TCL**: `SE_CNTL_STATUS.TCL_BYPASS` follows X.Org; wrong
   handling shows up as a failed probe.
3. **Hyper-Z** is never enabled (`Z_COMPRESSION`, `Z_HIERARCHY` off); R200
   depth needs `RB3D_DEPTHXY_OFFSET=0`.
4. **MC window**: 3D offsets use `engine.local_base`; the 2D driver already
   refuses a changed `MC_FB_LOCATION` and the probe refuses heaps outside
   the MC window.
5. **Throughput**: MMIO vertex submission costs 28 uncached writes per
   triangle plus FIFO polling; the f7-11 cost model applies with 28 words
   per triangle. No DMA or CP is added.
6. **VRAM readback** through the uncached aperture after every job, like the
   Savage; the same chunked deadlines apply.

## 9. Files

- `src/kernel/drivers/video/radeon2d/radeon3d_regs.h`: 3D registers and
  bits (Linux v6.1 `radeon_reg.h`, Mesa 7.11.2 `r200_reg.h`), notices in
  `docs/licenses/radeon2d-NOTICES.txt`.
- `src/kernel/drivers/video/radeon2d/radeon3d.h`, `radeon3d.c`: backend
  (layout, validate, enter/leave, execute, idle, quarantine, capabilities,
  minimum/full-size/precision probes).
- `src/kernel/drivers/video/radeon2d/radeon3d_owner.c`: owner glue (token,
  claims, mapping, marker, hooks `radeon3d_boot_diagnostic`,
  `radeon3d_bind_vfs`, `radeon3d_owner_stop`, `radeon3d_owner_attach`).
- `src/kernel/drivers/video/render3d_precision.h`: the boot precision
  oracle, family-neutral (previously `s3savage/savage3d_precision.h`, now a
  one-line include of it).
- `radeon2d/driver.c`: `render_family` becomes `R3D_UNQUALIFIED` with
  `.backend=radeon3d_backend` (weak), hooks wired like the Savage owner,
  attempt file `accel-3d-radeon-1002-%04x.cfg`; `engine.h` gains
  `render3d_qualified`.
- Tests: `tests/host/radeon3d_model.inc` (fake engine: FIFO/status model,
  register decoder, 2D fill/blit, rasterizer with parameterized sampling
  arithmetic), `tests/host/radeon3d_backend_test.c` (probe, variants, fixed
  function, full size, precision, fuzzing, containment, software fallback),
  `tests/host/radeon3d_owner_test.c`, `tests/host/test_radeon3d.py`;
  `tests/host/win32/gl/kernel_engine.c` family-switchable for the H1
  replay (`GL_KERNEL_FAMILY_RADEON`), `test_gl.py` builds the second binary.

## 10. Continuation audit and binding decisions

Audited 2026-10-28. The lead's D-F1/D-F3/D-F4 are other-family decisions.
D-F2: all state used here is read back; masked status bits are compared only
where documented. Pitch/offset restoration is checked, and the four 2D draw
registers are also saved and re-emitted. Every 2D operation programs its own
complete draw state. The minimum probe's hardware fills and copy check the
same 2D engine inside the claimed heap. If a future revision needs a write-only
baseline, it requires the D-F2 shadow and a qualified 2D self-check; no guessed
restore is substituted here.

D-F5: the unchanged R100/R200 CP binaries, upstream Radeon WHENCE section and
MIT notice are packaged under `/System/firmware/`. They are not loaded:
X.Org's ACCEL_MMIO path proves direct register submission, and the existing
2D owner requires `CP_CSQ_CNTL == 0`. Enabling CP or DMA would violate that
owner contract. Firmware provenance is in
`third_party/firmware/radeon/sources.json`; upstream linux-firmware commit
`505662b1f08ba6b5ecdc46da61bf5b30c8592698`.

D-F6: host commands run sequentially with `nice -n 19`; no scope or guest is
invoked. The existing synchronous record hook is still the orchestration in
this worktree. f7-13 background budget orchestration is not present here;
the lead must integrate that hook when it lands. No hardware claim follows
from a successful fake engine, and host execution time is not chip timing.

The 1/16-pixel coordinate restriction applies to every emitted vertex.
Varying reciprocal W is refused even in minimum mode until the perspective
probe passes. FIFO waits are count-bounded even if the clock freezes; elapsed
execution time is checked even when space is continuously available. Upload
and readback chunks are bounded to 1 ms, including guard initialization.
No public ABI or SDK header changes are required.

The fake engine interprets the written register addresses, vertex stream,
linear texture mip layout, combiner selectors, 2D fills/copies and depth/blend
state. It never calls production rasterization, state-building or sampling
helpers. Unknown silicon interpolation widths remain model parameters. The
full-size oracle uses the same shared integer envelope as the Savage; host
faults and altered arithmetic must refuse qualification rather than widen it.

## 11. Individual primary-source hashes

`L/` in this table is `linux-6.1-radeon/` (identical relevant files to the lead's
`linux-6.1-drm` v6.1 checkout), `M/` and `X/` are defined in §1. Mesa source
origin is [the 7.11.2 release](https://archive.mesa3d.org/older-versions/7.x/7.11.2/);
X.Org source origin is [ati 6.14.6](https://www.x.org/archive/individual/driver/xf86-video-ati-6.14.6.tar.bz2).
File:line references above identify the exact sections; hashes identify the
whole file, including copyright notices.

| File | SHA-256 |
| --- | --- |
| `L/radeon_reg.h` | `ed549525a1668899fd0c348ac6d1f52f3d9f4abc08d10387c8666cceaa5c552d` |
| `L/r100.c` | `5e3bdf7a84a086505dc59215467c26ab97eb000b29a46a32856a125c496b600d` |
| `L/r100d.h` | `1d0c10bec76065794ce6daf753d51ddc70d79a86a2ea52417963b0dea258866a` |
| `L/r200.c` | `7bb47ef0c76037c8b9930d002cb3e265f77580aa937ddd5a95786d42a4e0e5a9` |
| `L/r100_track.h` | `1814fde029725431a85313352a49ab623db80293f5b834d43f6e7c39ade4c3d5` |
| `L/radeon.h` | `f9ce62554fa026a135747808374ebf473cd1490ce020748a96db6b9bb4a39135` |
| `L/radeon_asic.h` | `f408e19d79c7c125088575e3ac35691c23b3c2c882edbe83ad970846496ef596` |
| `L/radeon_drm.h` | `4d1d189cab7ab4cf256d011f62ec43985fadcc16a995e0acb84e70b3ac8ff52f` |
| `M/radeon/radeon_screen.c` | `ecc660fdc5af64b85d34cf25aefa6461b026d49876187bbb53bd5756d6bc35e0` |
| `M/radeon/radeon_swtcl.c` | `bd13ee592b8dd053754cc76a71d8cd342869cc1b41b5a3d2c03c6f1dadd5fa82` |
| `M/radeon/radeon_state_init.c` | `6d9e2495908e58b8ffcd8713a6ccfc3598851384316b3ba576755764c19a21b6` |
| `M/radeon/radeon_state.c` | `6f81655af6ed5b9f4f26f2db73386646474b109660ab44bf4d8b81a44af5c475` |
| `M/radeon/radeon_texstate.c` | `85b9ee19b6a1baa6b77a3384c37cffe3f35bd8302321564648dd2ec125360440` |
| `M/radeon/radeon_mipmap_tree.c` | `05145ea50488a98ca155b23fd3c67db6bf82d329900b82200f767d68f9ad43e5` |
| `M/radeon/radeon_common_context.c` | `c919f333f24e816eadb9ce4aef2674deb3111393dc89bff3b2d09e13361bc834` |
| `M/radeon/radeon_tex.c` | `df2dda30bb07bb826cefb690c5548e283b27862841bb3fc83c23002587ea3947` |
| `M/radeon/radeon_ioctl.c` | `5cd8f335aa5270e1fa290189a3c6a83ff81902468d0fb8a12e5a6c99c307677e` |
| `M/radeon/radeon_context.c` | `1372a366b4efce7a2a3daf0b929f07d73f9bc6eaea7cefb7156e18517868756b` |
| `M/r200/r200_swtcl.c` | `6e34312a725533fc5eecb6d2513253095ffadf384dfe2ef46a40b24fb57218dc` |
| `M/r200/r200_state_init.c` | `75480d2f24a2b355176186b843464e4a3aada9b52c0a2e72d19953c067cfd34c` |
| `M/r200/r200_reg.h` | `5e2e40d3f5aa381dc225360e4c59fbe9205c502adb57eee38717480ba6bd0ccc` |
| `M/r200/r200_texstate.c` | `b2e0809f2e6e92359184e3f060208113572878e1bc7f354f34793dae8cd33311` |
| `M/r200/r200_tex.c` | `bbed02559de2fd0086718b8d028f4c34ff7dae844f9b796d125faed243eb896e` |
| `M/r200/r200_context.c` | `38b7233e5595d1eac582927aba776b4f0c60f90db653200a293f92f0567a6d8b` |
| `X/radeon_exa_render.c` | `31d09e46ff7dbb7b6c432e971423114788fc6aa52e2d2050bd3ea6c314d7f32c` |
| `X/radeon_commonfuncs.c` | `04276b76e298f23271f4bcfa58dd64b318cb7305e195634d3ea94dda4e238518` |
| `X/radeon_accel.c` | `9d9df80ef75e10695efa4c263327ae9ef69fb65cdfce5c74219a971ee98e71b0` |
| `X/radeon_exa.c` | `712f5219ac4dd1870e8951533ad3020042c5694d83910b1652e93bdf384d8711` |
| `X/radeon_exa_funcs.c` | `e70bfd453ea0e440cf5a63dc49236e23ddeb6cf9697bec7ff27034122c9b9ed0` |
| `linux-firmware/radeon/R100_cp.bin` | `ed3acccf755c7270dd8893605f44ff8b9e7f6c542f66ed0738c9fb7bbbd75d7c` |
| `linux-firmware/radeon/R200_cp.bin` | `2b7b4bf988c3f7c69f98ea2ec6c4155fa85e2c9d5f5aee77912baaf50f5f7bac` |
| `linux-firmware/WHENCE` | `ee6fcbd18f7b69d331ade28de5a7b0b9ba68220e86f26d445679a3f939906353` |

## 12. Register address audit

Each address written by the backend is covered below. Consecutive texture
units follow the explicit upstream `_1` addresses (R100 0x18 and R200 0x20,
with R200 offsets 0x18 and combiners 0x10). Bit encodings and the MMIO FIFO
protocol are cited in §§3–4; no command-processor packets are emitted.

| Register | Upstream file:line |
| --- | --- |
| `R200_PP_BORDER_COLOR_0` | `M/r200/r200_reg.h:961` |
| `R200_PP_CNTL_X` | `L/radeon_reg.h:3280` |
| `R200_PP_TFACTOR_0` | `L/radeon_reg.h:2942` |
| `R200_PP_TXABLEND2_0` | `L/radeon_reg.h:3190` |
| `R200_PP_TXABLEND_0` | `L/radeon_reg.h:3094` |
| `R200_PP_TXCBLEND2_0` | `L/radeon_reg.h:3048` |
| `R200_PP_TXCBLEND_0` | `L/radeon_reg.h:2949` |
| `R200_PP_TXFILTER_0` | `L/radeon_reg.h:2758` |
| `R200_PP_TXFORMAT_0` | `L/radeon_reg.h:2813` |
| `R200_PP_TXFORMAT_X_0` | `L/radeon_reg.h:2864` |
| `R200_PP_TXMULTI_CTL_0` | `L/radeon_reg.h:3281` |
| `R200_PP_TXOFFSET_0` | `L/radeon_reg.h:2892` |
| `R200_PP_TXPITCH_0` | `L/radeon_reg.h:2878` |
| `R200_PP_TXSIZE_0` | `L/radeon_reg.h:2871` |
| `R200_RB3D_DEPTHXY_OFFSET` | `M/r200/r200_reg.h:298` |
| `R200_RE_AUX_SCISSOR_CNTL` | `M/r200/r200_reg.h:819` |
| `R200_RE_CNTL` | `L/radeon_reg.h:2447` |
| `R200_RE_SCISSOR_BR_0` | `M/r200/r200_reg.h:292` |
| `R200_RE_SCISSOR_TL_0` | `M/r200/r200_reg.h:291` |
| `R200_SE_VAP_CNTL` | `L/radeon_reg.h:2732` |
| `R200_SE_VTE_CNTL` | `L/radeon_reg.h:2742` |
| `R200_SE_VTX_FMT_0` | `L/radeon_reg.h:3228` |
| `R200_SE_VTX_FMT_1` | `L/radeon_reg.h:3256` |
| `R200_SE_VTX_STATE_CNTL` | `L/radeon_reg.h:3287` |
| `RADEON_COLORPITCH_MASK` | `L/radeon_reg.h:2282` |
| `RADEON_DEPTHPITCH_MASK` | `L/radeon_reg.h:2290` |
| `RADEON_DP_BRUSH_FRGD_CLR` | `L/radeon_reg.h:734` |
| `RADEON_DP_CNTL` | `L/radeon_reg.h:735` |
| `RADEON_DP_GUI_MASTER_CNTL` | `L/radeon_reg.h:748` |
| `RADEON_DST_HEIGHT_WIDTH` | `L/radeon_reg.h:839` |
| `RADEON_DST_PITCH_OFFSET` | `L/radeon_reg.h:849` |
| `RADEON_DST_WIDTH_HEIGHT` | `L/radeon_reg.h:857` |
| `RADEON_DST_Y_X` | `L/radeon_reg.h:865` |
| `RADEON_PP_BORDER_COLOR_0` | `L/radeon_reg.h:1864` |
| `RADEON_PP_CNTL` | `L/radeon_reg.h:1867` |
| `RADEON_PP_FOG_COLOR` | `L/radeon_reg.h:1897` |
| `RADEON_PP_MISC` | `L/radeon_reg.h:1905` |
| `RADEON_PP_TEX_PITCH_0` | `L/radeon_reg.h:2093` |
| `RADEON_PP_TEX_SIZE_0` | `L/radeon_reg.h:2082` |
| `RADEON_PP_TFACTOR_0` | `L/radeon_reg.h:2221` |
| `RADEON_PP_TXABLEND_0` | `L/radeon_reg.h:2182` |
| `RADEON_PP_TXCBLEND_0` | `L/radeon_reg.h:2098` |
| `RADEON_PP_TXFILTER_0` | `L/radeon_reg.h:1931` |
| `RADEON_PP_TXFORMAT_0` | `L/radeon_reg.h:1984` |
| `RADEON_PP_TXOFFSET_0` | `L/radeon_reg.h:2051` |
| `RADEON_RB3D_BLENDCNTL` | `L/radeon_reg.h:2225` |
| `RADEON_RB3D_CNTL` | `L/radeon_reg.h:2254` |
| `RADEON_RB3D_COLOROFFSET` | `L/radeon_reg.h:2279` |
| `RADEON_RB3D_COLORPITCH` | `L/radeon_reg.h:2281` |
| `RADEON_RB3D_DEPTHOFFSET` | `L/radeon_reg.h:2288` |
| `RADEON_RB3D_DEPTHPITCH` | `L/radeon_reg.h:2289` |
| `RADEON_RB3D_DSTCACHE_CTLSTAT` | `L/radeon_reg.h:1627` |
| `RADEON_RB3D_PLANEMASK` | `L/radeon_reg.h:2294` |
| `RADEON_RB3D_ROPCNTL` | `L/radeon_reg.h:2295` |
| `RADEON_RB3D_STENCILREFMASK` | `L/radeon_reg.h:2313` |
| `RADEON_RB3D_ZCACHE_CTLSTAT` | `L/radeon_reg.h:1610` |
| `RADEON_RB3D_ZSTENCILCNTL` | `L/radeon_reg.h:2320` |
| `RADEON_RBBM_STATUS` | `L/radeon_reg.h:1598` |
| `RADEON_RE_MISC` | `L/radeon_reg.h:2382` |
| `RADEON_RE_TOP_LEFT` | `L/radeon_reg.h:2391` |
| `RADEON_RE_WIDTH_HEIGHT` | `L/radeon_reg.h:2394` |
| `RADEON_SE_CNTL` | `L/radeon_reg.h:2401` |
| `RADEON_SE_CNTL_STATUS` | `L/radeon_reg.h:2459` |
| `RADEON_SE_COORD_FMT` | `L/radeon_reg.h:2465` |
| `RADEON_SE_PORT_DATA0` | `L/radeon_reg.h:2730` |
| `RADEON_SE_VF_CNTL` | `L/radeon_reg.h:2703` |
| `RADEON_SE_VTX_FMT` | `L/radeon_reg.h:2678` |
| `RADEON_SRC_PITCH_OFFSET` | `L/radeon_reg.h:1709` |
| `RADEON_SRC_Y_X` | `L/radeon_reg.h:1716` |
| `RADEON_DP_WRITE_MSK` (upstream `RADEON_DP_WRITE_MASK`) | `L/radeon_reg.h:832` |
