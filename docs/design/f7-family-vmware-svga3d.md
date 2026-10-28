# VMware SVGA II: SVGA3D (legacy VGPU9) fixed-function backend

Research date: 2026-10-26 (`date` on the workstation). Directive:
[f7-31](../directives/f7-31-vmware-svga3d.md); umbrella rules: the f7-20
[f7-20 directive](../directives/f7-20-other-families.md) and the VMware
section of [f7-3d-acceleration-contract.md](f7-3d-acceleration-contract.md). This is a source-derived implementation with a strict host
model; it is not a qualification on a real hypervisor. No QEMU, VirtualBox or
VMware session was run for this document. The VMware owner retains its
original synchronous `record` hook; integration 6's Savage background worker
is preserved without changes.
The resumption audit on 2026-10-28 (`date` checked) applies the lead's
binding D-F2 restoration/self-check rule and corrects the VirtualBox 2D
prerequisite and marker-clear ordering described below.

## 1. Sources and provenance

The pinned local primary-source files are preserved under
`build/downloads/f7-sources/`; `SOURCES.sha256` there lists all 70 files
with their hashes, paths and primary URLs. The table below lists the cited
files. The resumption audit rechecks the local hashes against that manifest. Paths in the text are abbreviated: `mesa:` =
Mesa 21.3.9 `src/gallium/drivers/svga/include/`, `mesa-svga:` = Mesa 21.3.9
`src/gallium/drivers/svga/`, `linux:` = Linux v6.1
`drivers/gpu/drm/vmwgfx/device_include/`, `linux-vmwgfx:` = Linux v6.1
`drivers/gpu/drm/vmwgfx/`, `vbox:` = VirtualBox commit
`b6bea72111fcff48f31c57c9f66365b1d3301bb9` `src/VBox/Devices/Graphics/`.

| File | Primary URL | SHA-256 |
| --- | --- | --- |
| mesa: svga3d_cmd.h | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/include/svga3d_cmd.h) | `bc3b1ea26b93406c89b1a5ff4847f9457aac4f816b284342af90f56dd0ad8885` |
| mesa: svga3d_types.h | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/include/svga3d_types.h) | `c3458f410f5b238a2ffb852988a11a9684e7a05dc48aecfb45f2a050f3fa12f7` |
| mesa: svga3d_devcaps.h | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/include/svga3d_devcaps.h) | `1a7c2439d7ff429e34062481745dcf206644da5605ef323e2cdd91382e011ee0` |
| mesa: svga3d_limits.h | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/include/svga3d_limits.h) | `44fe920f208a56612c60b7372b772e217197db7093938b65360da7872bd70bda` |
| mesa: svga_reg.h | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/include/svga_reg.h) | `a468afdc04c43fb902fd6b1f00375db66cb1605bfdd2022476f5d74fc02288e3` |
| mesa-svga: svga_cmd.c | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/svga_cmd.c) | `6b1a652e4f6dff13f152023e947ecb74fbb7abdf9bedbfa567e00d663a7925d6` |
| mesa-svga: svga_state_rss.c | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/svga_state_rss.c) | `0287d1778a6790a1d74c2185cc7f6a9b9cf06cedcccdc53654e28ea7f7d2dd99` |
| mesa-svga: svga_state_tss.c | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/svga_state_tss.c) | `ea7b3c9d5fde0b776c851438ba7487eb45d4c44f72393ae634f21d1cad6c7315` |
| mesa-svga: svga_screen.c | [mesa-21.3.9](https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-21.3.9/src/gallium/drivers/svga/svga_screen.c) | `773aceb065dfb40b73b236827a389bb52a5034fd01399a6549d68126ce4d71bc` |
| linux: svga3d_cmd.h | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/device_include/svga3d_cmd.h) | `be97542a60591fccfaf40aaf72738d8ad8a5e7218404410850ab234eb513152c` |
| linux: svga3d_types.h | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/device_include/svga3d_types.h) | `0e8b4172666e280837b87fbf8ac7ff8d76b09c3fc96a419688a3569779f80bff` |
| linux: svga3d_devcaps.h | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/device_include/svga3d_devcaps.h) | `847248f70f01fdf41f3cea3b82a3ba823a48ed1994b73052452d21393802fd63` |
| linux: svga3d_limits.h | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/device_include/svga3d_limits.h) | `b3f5e85c61b6795cbadfdb2ada87a4bffd59229a090bed022545067bd74e2191` |
| linux: svga_reg.h | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h) | `1303b1df3983ea310862d1187a0d01797f3511fa6fc6bf406c9bb779beb4c4a4` |
| linux-vmwgfx: vmwgfx_cmd.c | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/vmwgfx_cmd.c) | `0687b061792c1aed5aa7f5c346b100fe8b58401939dc5fd64b01d375f2911080` |
| linux-vmwgfx: vmwgfx_devcaps.c | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/vmwgfx_devcaps.c) | `98393a1fe2d6ff44844f15365e45654bdef019b235f91c0e52ed9c121d177ff1` |
| linux-vmwgfx: vmwgfx_surface.c | [v6.1](https://raw.githubusercontent.com/torvalds/linux/v6.1/drivers/gpu/drm/vmwgfx/vmwgfx_surface.c) | `57237ed1807829656769123f18abdb36bfdf462093a69ce5ed57fa330ec48e6b` |
| vbox: DevVGA.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA.cpp) | `169c2d0bdf842a906917ba0a4d57ca5e3154bed273a5b739e7360251b24618a3` |
| vbox: DevVGA-SVGA.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA.cpp) | `61aac563fa517843a6438e47cc4fc08fe74a5b95597235280d0018e6c98a0c6d` |
| vbox: DevVGA-SVGA-cmd.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA-cmd.cpp) | `796d641681de3f68478bcfc92bf8ba43848e528f0e2d88dff3c1303a6e366529` |
| vbox: DevVGA-SVGA3d.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA3d.cpp) | `fc35dab99b7f6cb4012b908c9c618add19dc8693854dad5936d7d55f4b4eb633` |
| vbox: DevVGA-SVGA3d-ogl.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA3d-ogl.cpp) | `ac21bedb026468bfa990be4322575456d6b52655a9a15fac17fad913b0522fd1` |
| vbox: DevVGA-SVGA3d-dx-dx11.cpp | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/DevVGA-SVGA3d-dx-dx11.cpp) | `c3aab68faa399ee9fe36db58c94ff2c6e19785749df50d95de54ad72f7961de7` |
| vbox: vmsvga_include/svga3d_caps.h | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/vmsvga_include/svga3d_caps.h) | `89224d516c14af1254d685a1a7d472f9d346b9f21b767df740ce9d477dd435c4` |
| vbox: shaderlib/shaderapi.c | [b6bea72](https://raw.githubusercontent.com/VirtualBox/virtualbox/b6bea72111fcff48f31c57c9f66365b1d3301bb9/src/VBox/Devices/Graphics/shaderlib/shaderapi.c) | `0641f5c09ead00bf5eda4edf3577252aea022ef8f35c071b75885659d5dbd267` |
| QEMU hw/display/vmware_vga.c | [v9.2.0](https://gitlab.com/qemu-project/qemu/-/raw/v9.2.0/hw/display/vmware_vga.c) | `f1f485237de897949ccc47daef71c4e3883e5bdcfc11bd3891ae5d1a98efbdbf` |

The VMware SVGA developer kit (SourceForge `vmware-svga`) could not be
fetched (its git and raw endpoints return HTML or 404 from this host); the
Mesa copies of the VMware headers carry the same MIT documentation comments.
No upstream code is vendored. `vmwaresvga3d_protocol.h` holds only numeric
values, each with its `mesa:` and `linux:` line;
`tests/host/test_vmwaresvga3d.py::test_protocol_citations` re-reads every
cited line (126 constants, 252 citations) when the pinned files are present.

## 2. Identity, capability and hardware-version gates

The existing 2D table is authoritative: 15ad:0405 only (`vmwaresvga/driver.c`),
the shared FIFO engine and its validated ring (`engine.c`). The family
descriptor moves from `R3D_NOT_IMPLEMENTED` to `R3D_UNQUALIFIED` with
`vmsvga3d_backend`; that alone admits nothing.

Gates, all before any 3D write (`vmsvga3d_read_caps`, owner preflight):

1. `SVGA_CAP_3D` (mesa: svga_reg.h:635, linux: svga_reg.h:409). QEMU v9.2.0
   reports only RECT_COPY/RECT_FILL/cursor bits (`vmware_vga.c:898-911`) and
   its FIFO decoder knows only 2D commands 0-30, treating anything else as an
   unknown command (`vmware_vga.c:630-790`). The owner refuses QEMU with no
   port I/O at all (`reason=no_svga3d`).
2. Extended FIFO with registers through `SVGA_FIFO_3D_CAPS_LAST`
   (mesa: svga_reg.h:811-812) and `SVGA_FIFO_CAP_FENCE` (mesa: svga_reg.h:1063).
3. Hardware version. Legacy hosts: `SVGA_FIFO_3D_HWVERSION_REVISED` when
   `SVGA_FIFO_CAP_3D_HWVERSION_REVISED` is set, else `SVGA_FIFO_3D_HWVERSION`,
   minimum `SVGA3D_HWVERSION_WS8_B1` = MAKE_HWVERSION(2,1) (mesa:
   svga3d_devcaps.h:52-64). Justification: Linux v6.1 `vmw_supports_3d`
   refuses anything older (linux-vmwgfx: vmwgfx_cmd.c:56-75) and it is exactly
   what VirtualBox reports (vbox: DevVGA-SVGA.cpp:8482-8483). Hosts with
   `SVGA_CAP_GBOBJECTS` (mesa: svga_reg.h:648) gate on `SVGA3D_DEVCAP_3D`
   instead (vmwgfx_cmd.c:43-51).
4. Devcaps. Legacy hosts: the bounded `SVGA3dCapsRecord` block at
   `SVGA_FIFO_3D_CAPS` (vbox: vmsvga_include/svga3d_caps.h:66-109; header
   length in words, DEVCAPS record types 0x100..0x1ff, zero length ends; at
   most 64 records, none past `SVGA_FIFO_3D_CAPS_LAST`). GB hosts: the
   `SVGA_REG_DEV_CAP` index/value register (mesa: svga_reg.h:187,
   linux-vmwgfx: vmwgfx_devcaps.c:85-100); Linux only synthesizes the FIFO
   record for such hosts (:126-140), so the block may be empty. The register
   path uses the shared index port and therefore runs inside
   `fbdev_render3d_enter`.
5. Required devcaps (mesa: svga3d_devcaps.h:73-188): `MAX_TEXTURES>=1`
   (two stages need >=2), `D16_BUFFER_FORMAT`, `MAX_TEXTURE_WIDTH/HEIGHT>=64`,
   `MAX_PRIMITIVE_COUNT>=256`, `MAX_VERTEX_INDEX>=767`,
   `SURFACEFMT_A8R8G8B8` with TEXTURE|OFFSCREEN_RENDERTARGET and
   `SURFACEFMT_Z_D16` with ZSTENCIL (mesa: svga3d_types.h:656-661),
   `MAX_CONTEXT_IDS>1`, `MAX_SURFACE_IDS>22`, and a present `DEVCAP_3D` must be
   nonzero. `TEXTURE_OPS` (devcap 31) gates the optional combiners (section 6).

Nothing is hard-coded per host: units, texture size limits and combiners come
from these answers and from this boot's Q1-Q4.

## 3. What VirtualBox implements and refuses

* **2D prerequisite:** the inspected VirtualBox caps omit legacy FILL
  (vbox: DevVGA-SVGA.cpp:8079-8091). The existing CiukiOS 2D owner requires
  that bit (`engine.c`, `native_fill_unavailable`) and therefore never binds
  there. The 3D model deliberately supplies this prerequisite to isolate
  VGPU9 semantics; its successful GL qualification and H1 prefix do not
  establish that an actual VirtualBox guest reaches the probe. Extending the
  2D binding needs a lead directive; this backend preserves its gate.
* Backend choice. `VMSVGA10` defaults to true (vbox: DevVGA.cpp:6898); then
  the DX backend is used, otherwise the legacy OpenGL one (vbox:
  DevVGA-SVGA.cpp:8016-8023). 3D itself defaults off (`VMSVGA3dEnabled`,
  DevVGA.cpp:6927).
* DX backend (default VirtualBox 7): its VGPU9 table exists
  (vbox: DevVGA-SVGA3d-dx-dx11.cpp:14717-14760) but `ContextDefine` returns
  `VERR_NOT_IMPLEMENTED` (:5578-5586) and SetRenderState, SetRenderTarget,
  SetTextureState, Clear, DrawPrimitives, SetViewPort, SetZRange and
  SetTransform are no-op stubs that return success (:5609-5760). On the FIFO
  path the 3D handler's status is ignored (vbox: DevVGA-SVGA.cpp:6478-6505),
  so the FIFO survives; nothing renders. Q1 therefore fails on coverage and the
  backend refuses cleanly (2D retained). VGPU9 cannot work there; it needs
  VGPU10 DX contexts, guest-backed objects and DXBC shaders (not in scope).
* Legacy OpenGL backend (`VMSVGA10=0`): implements contexts, surfaces, DMA,
  render targets, render/texture states and draws through wined3d-derived
  fixed function (vbox: DevVGA-SVGA3d-ogl.cpp). Reported facts the design
  relies on:
  * `SVGA_CAP_3D` when a 3D interface exists (DevVGA-SVGA.cpp:8133), FIFO
    caps FENCE and 3D_HWVERSION_REVISED (:8138-8143), hardware version
    `SVGA3D_HWVERSION_CURRENT` = WS8_B1 in both registers (:8482-8483) and a
    DEVCAPS record covering devcaps 0..83 (:8486-8506); with VGPU10 enabled
    it also sets `SVGA_CAP_GBOBJECTS`/`SVGA_CAP_DX` (:8097-8122).
  * `TEXTURE_OPS` = 0 (DevVGA-SVGA3d-ogl.cpp:1526-1527); COLOROP/COLORARG/
    ALPHAOP/ALPHAARG are ignored ("not yet supported", :5687-5698), so every
    enabled unit uses GL's default MODULATE. MODULATE (both stages) and the
    emulated stage-0 REPLACE qualify; ADD, DECAL and stage-1 REPLACE are never
    attempted there.
  * `MAX_TEXTURES` from `GL_MAX_TEXTURE_UNITS_ARB` (:961), texture sizes
    from `GL_MAX_RECTANGLE_TEXTURE_SIZE` (:970), D16 buffer/format support
    (:1454, :1599).
  * `SVGA_GMR_FRAMEBUFFER` DMA copies straight from/to VRAM with offset and
    pitch checks (vbox: DevVGA-SVGA-cmd.cpp:8660-8800); surface DMA works for
    texture and buffer resources only (DevVGA-SVGA3d-ogl.cpp:2881-3175).
    Colour and depth render targets are created as textures (:5336-5353,
    :5408-5414), so their readback works.
  * POSITION decls go to `glVertexPointer(size, ...)` (:6893-6900) and
    D3DCOLOR to `GL_BGRA` bytes (`vmsvga3dVertexDecl2OGL`); untransformed
    vertices use `ShaderTransformProjection(..., false)` with a
    63/64-pixel offset and z mapped [0,1]->[-1,1] (vbox: shaderapi.c:741-799).
  * Not used, recorded as refusals: the pretransformed POSITIONT path
    projects with `glOrtho(0,w,0,h,-1,1)` after a -1 z translation
    (shaderapi.c:782-791), which puts z>0 outside the clip volume, and passes
    the fourth component as GL w; it is also only recomputed when the
    transformed flag toggles (shaderapi.c:659-686). This is why the backend
    uses POSITION with a projection matrix instead of POSITIONT.
* Writing `SVGA_REG_ENABLE` with the same value (the 2D engine's FIFO
  disable proof) raises a mode change (DevVGA-SVGA.cpp:2147-2176) whose 3D
  effect on contexts is a no-op in the legacy backend
  (DevVGA-SVGA3d-ogl.cpp:4147-4172); `SVGA_REG_CONFIG_DONE` toggles FIFO
  processing and tracing only (DevVGA-SVGA.cpp:2288-2296). The persistent
  context survives the 2D engine's per-batch FIFO enable/disable.

VirtualBox is an implementation, not a specification; this section is what
the boot qualification is expected to find there.

## 4. Encoding: fixed function with a projection, no shaders

* Vertex buffer: one `SVGA3D_BUFFER` surface (mesa: svga3d_types.h:219,
  `SVGA3D_SURFACE_HINT_VERTEXBUFFER` :359) with nine dwords per vertex:
  POSITION FLOAT4 (X,Y,Z,W) at 0, D3DCOLOR at 16, TEXCOORD0 FLOAT2 at 20,
  TEXCOORD1 FLOAT2 at 28, stride 36 (decl types mesa: svga3d_types.h:1405-1424,
  usages :1377-1387; `SVGA3dVertexDecl` mesa: svga3d_cmd.h:649-722).
* Positions are homogeneous. A triangle with one reciprocal W emits
  (x, y, z/65535, 1). A triangle with varying W emits (x, y, z/65535)/rhw and
  W = 1/rhw: the host's clip divide restores screen x, y, z, and texture
  coordinates are interpolated with the request's reciprocal W, which is the
  render3d contract (`render3d_scalar.c` coordinate()).
* Projection (`SVGA_3D_CMD_SETTRANSFORM` 1047, mesa: svga3d_cmd.h:68, :937-945;
  WORLD/VIEW = identity, PROJECTION = P, mesa: svga3d_types.h:1518-1522),
  row-vector convention: x_ndc = 2(x-1/2)/W - 1, y_ndc = 1 - 2(y-1/2)/H,
  z_ndc = z. D3D9 puts pixel centres on integer viewport coordinates; the
  half-pixel term maps render3d's x+0.5 sample positions onto them.
  VirtualBox reaches the same convention through its 63/64 offset.
* Every float is produced by integer arithmetic only (no FPU in the kernel):
  `vmsvga3d_ratio_ieee` is a correctly rounded binary32 of an exact rational
  (round-half-to-even); 200,000 host-checked cases.
* Colour: the host interpolates diffuse perspective-correctly, the render3d
  contract affinely; the validator therefore refuses (software for the whole
  context) a varying-W triangle whose three colours differ, unless flat.
  Flat shading writes the flat colour into all three vertices.

## 5. Commands, render states and texture states

| Use | Command (id) | Layout | Mesa emitter | VirtualBox handler |
| --- | --- | --- | --- | --- |
| context | CONTEXT_DEFINE 1045 / DESTROY 1046 (mesa: svga3d_cmd.h:66-67) | :425-439 | svga_cmd.c:166, :200 | cmd.cpp:5769, :5779 |
| surfaces | SURFACE_DEFINE 1040 / DESTROY 1041 (:61-62) | :351-385 + SVGA3dSize, :417-423 | :274, :367 | :5632, :5668 |
| upload/readback | SURFACE_DMA 1044 (:65) with suffix | :552-632 | :428-485 | :5700, DevVGA-SVGA3d.cpp:760-905 |
| copy | SURFACE_COPY 1042 (:63) | :515-523 | :1054 | :5678 |
| targets | SETRENDERTARGET 1050 (:71), COLOR0/DEPTH (mesa: svga3d_types.h:1161-1164) | :505-513 | :586 | :5820 |
| viewport/z | SETVIEWPORT 1055 (:76), SETZRANGE 1048 (:69) | :1007-1014, :955-963 | :1142, :1258 | :5871, :5799 |
| transforms | SETTRANSFORM 1047 (:68) | :937-945 | (Mesa uses shaders) | :5789 |
| states | SETRENDERSTATE 1049, SETTEXTURESTATE 1051 (:70, :72) | :485-503, :925-935 | :1351, :1305 | :5809, :5830 |
| clear | CLEAR 1057 (:78), COLOR/DEPTH flags (mesa: svga3d_types.h:1148-1149) | :441-452 | :894 | :5891 |
| draw | DRAW_PRIMITIVES 1063 (:84), TRIANGLELIST (mesa: svga3d_types.h:1463) | :649-760 | :993 | :5961 |
| fence | SVGA_CMD_FENCE 30 (mesa: svga_reg.h:1301, :1508-1525) | 1 dword | linux-vmwgfx: vmwgfx_cmd.c:520-547 | DevVGA-SVGA.cpp:6226-6234 |

Render states, set in full every job (mesa: svga3d_types.h:829-932; Mesa
mapping svga_state_rss.c:111-239): ZENABLE (test or write), ZWRITEENABLE,
ZFUNC (ALWAYS for write-only depth, since D3D9 ZENABLE=0 also disables
writes), ALPHATESTENABLE, ALPHAFUNC, ALPHAREF as float ref/255 (:870),
BLENDENABLE, SRCBLEND, DSTBLEND (:1000-1009), BLENDEQUATION=ADD (:1025),
SEPARATEALPHABLENDENABLE=0 (the same factors apply to alpha, as in the
scalar), DITHERENABLE/FOGENABLE/SPECULARENABLE/STENCILENABLE/LIGHTINGENABLE=0,
SHADEMODE=SMOOTH, CULLMODE=NONE (:1063), COLORWRITEENABLE=0xf,
SCISSORTESTENABLE=0. Compare functions map one to one (:1077-1084).

Texture stage states per stage (mesa: svga3d_types.h:1216-1229; Mesa
svga_state_tss.c:361-375): BIND_TEXTURE (sid or INVALID), COLOROP/ARG1/ARG2,
ALPHAOP/ARG1/ARG2, ADDRESSU/V = WRAP or CLAMP (:1298-1300), MAG/MINFILTER =
NEAREST or LINEAR, MIPFILTER = NONE / NEAREST (nearest mip) / LINEAR
(trilinear) (:1312-1315), TEXCOORDINDEX = stage. A disabled stage is
COLOROP=ALPHAOP=DISABLE with no texture. All bindings and render targets are
reset to INVALID before the job's surfaces are destroyed.

## 6. Combine modes

Stage programs use D3D9 texture-stage semantics (combiners mesa:
svga3d_types.h:1262-1293, arguments :1345-1352); stage 0's current input is
DIFFUSE, stage 1's is PREVIOUS. The scalar contract keeps source alpha for
RGB-only formats and source RGB for alpha-only formats.

| Mode | Encoding | Gate |
| --- | --- | --- |
| MODULATE | COLOROP=ALPHAOP=MODULATE(TEXTURE, current) | Q1 (incl. explicit alpha-modulate check: D3D9's stage-0 default ALPHAOP is SELECTARG1) |
| REPLACE, stage 0 | MODULATE with the diffuse channels forced to 1.0 (all, RGB or alpha per format class); x*1.0 is exact | `REPLACE0` phase (three formats) |
| REPLACE, stage 1 | SELECTARG1 (TEXTURE or PREVIOUS per format class) | TEXTURE_OPS SELECTARG1 + `REPLACE1` phases |
| ADD | COLOROP=ADD, ALPHAOP=MODULATE; alpha-only textures need no combiner | TEXTURE_OPS ADD|MODULATE + `ADD` phases |
| DECAL | COLOROP=BLENDTEXTUREALPHA, ALPHAOP=SELECTARG1(current); RGB-only = REPLACE; alpha-only refused | TEXTURE_OPS BLENDTEXTUREALPHA|SELECTARG1 + `DECAL` phases |

Capability snapshot `combine_modes`: MODULATE always; REPLACE when REPLACE0
qualified (stage-1 REPLACE without REPLACE1 is refused per job by the
validator and retires that context to software); ADD and DECAL when qualified.
Texture uploads are the canonical ARGB8888 shadows for all six formats.

## 7. Transport, fences, ownership

* One synchronous batch per job through the existing ring
  (`vmsvga_engine_submit3d`): same idle/tail proof as 2D packets, CONFIG_DONE
  enable, words written with wrap, NEXT_CMD, SYNC, then the engine appends
  `SVGA_CMD_FENCE` with a nonzero 32-bit sequence. Completion requires BUSY=0,
  STOP==NEXT and `SVGA_FIFO_FENCE` equal to that value; a zero or stale value
  never completes (protocol fault, quarantine). FIFO disable and ENABLE
  re-assertion follow as for 2D.
* Waits: jobs 250 ms, context define/destroy 1 s (`VMSVGA_WAIT3D_LIMIT_US`),
  10 us polls, poll-count bound independent of the clock, the presenter pause
  hook every millisecond. Expiry quarantines the shared owner (both domains).
* CPU staging copies are chunked; a 256-word chunk taking more than 1 ms is a
  dead-mapping timeout. No AGP, GMR or host-memory ring (contract rule B.7):
  only `SVGA_GMR_FRAMEBUFFER` guest pointers into the claimed VRAM.
* Ownership: every job runs between `fbdev_render3d_enter/leave` (presenter
  plus a drained 2D engine). The owner's stop path runs under the presenter
  from the 2D runtime stop, before FIFO and port mappings are revoked.

## 8. Memory plan, surfaces and containment

* Heap: VRAM above the scanout lease (`fbdev` claims exactly
  [phys, phys+size); SVGA II scans out from offset 0), bounded by
  min(firmware VRAM, `SVGA_REG_VRAM_SIZE`), reduced to the largest run free
  of every live MMIO claim, capped at plan + 8 MiB. Plan: backend scratch
  (guard, 27,648-byte vertex image, guard), two colour targets, D16, 256 KiB
  minimum texture pool (480 KiB needed for Q2/Q3), all page aligned with a
  guard page before and after every extent. One registry claim
  `vmwaresvga-3d`; only the owner maps it.
* Images are tight (`pitch = width*bpp`); D16 is the canonical uint16 depth
  (D3D9 LESS = smaller is nearer, no inversion); colour and textures are the
  canonical ARGB8888 bytes (D3DCOLOR/A8R8G8B8 little-endian).
* Host objects: one persistent context (cid 1) per qualified generation; per
  job, surfaces with fixed ids 16..21 (colour, depth, texture 0, copy,
  texture 1, vertex buffer) defined, filled, used, read back, unbound and
  destroyed in the same batch, before the fence. No host surface outlives a
  job; the model asserts zero live surfaces after every batch.
* Uploads: a non-resident resource is copied from its shadow into staging;
  staging then remains the device copy (readback refreshes it), so resident
  colour/depth/texture data is not re-copied by the CPU. Texture eviction is
  the service's LRU over the pool; an evicted texture is re-staged.
* DMA containment: every guest image lies inside the claim and outside its
  guards (validated extents); the DMA suffix carries `maximumOffset` = image
  bytes (mesa: svga3d_cmd.h:575-632; Linux vmwgfx_surface.c:295-340 does the
  same). Guards are checked after every job; damage is a fault.
* Saved state: the 3D context state is private and re-programmed in full
  every job; shared FIFO enable/disable follows section 7. D-F2 applies:
  every completed batch
  re-emits the driver's shadowed baseline (unbound targets/textures,
  disabled depth/alpha/blend, identity transforms and mode viewport), using
  the already cited context-scoped commands and states. This owner is the
  only writer; no 2D state is part of that private context (vbox:
  DevVGA-SVGA3d-ogl.cpp:3266-3300, :4126-4138). Stop destroys the context,
  then releases the claim. On a shared 2D fault the consumer is marked lost
  before close/collection, so admission closes and hardware shadows stay
  charged. A lost device keeps marker, claim and shadows
  (quarantined); no writes are retried without a drain proof.

## 9. Boot qualification Q1-Q4

Runs from the vmwaresvga `record` hook when the boot options contain
`3d=probe`, after the 2D engine is active; all scratch allocation and scalar
reference rendering happen outside presenter ownership. Order: QEMU gate,
presenter pin, caps under `enter`, plan/claim/mapping, scratch, durable marker
`3d-vmwaresvga` (`accel-3d-vmwaresvga-15ad-0405.cfg`, declared as the 2D
driver's second attempt file so the Control Panel retry clears it), then:

* Q1: context define; the Savage minimum scene on a 64x64 A8R8G8B8+D16 target
  (textured triangle, LESS rejection of a farther triangle, Gouraud sample
  +/-2, D16 +/-1, clear, copy into a linear buffer, copy==target everywhere)
  plus the MODULATE alpha check.
* Q2: the four mode-sized f7-11 scenes, sampled rows, +/-2 / +/-1, coverage
  and copy equality.
* Q3: the 23 f7-11 precision phases with the family-neutral envelope
  (`vmwaresvga3d_precision.h`, verbatim copy of `savage3d_precision.h` per
  section E), then the combiner phases of section 6. A numeric (-EILSEQ) or
  budget (-ETIMEDOUT, 30 s total) refusal reduces claims only.
* Q4: the owner grants admission for this boot, generation and mode only
  after the marker is cleared; nothing persists a qualified state.

Before clearing the marker, any refused context is destroyed and the
existing 2D owner's fill/copy activation comparison runs again under
presenter ownership. Its saved 16x16 framebuffer block is restored before
leaving (the existing 2D self-test authorization, not a 3D presentation).
It uses legacy RECT_FILL and RECT_COPY (vbox: DevVGA-SVGA.cpp:6335-6348;
QEMU vmware_vga.c:644-680), never changes scanout registers and never lends
the display lease to a 3D surface. This implements D-F2's 2D self-check.
An unsafe/quarantined transport cannot run a self-check; it keeps the marker
and both domains quarantined instead. Context-destroy or self-check failure
also retains the marker. A marker-clear failure closes admission and tears
down a healthy context without retrying persistence.

Failures: a numeric mismatch with a drained engine refuses cleanly (context
destroyed, 2D checked, claim released, marker cleared, 2D retained). Fence timeout,
protocol fault or guard damage quarantine both domains and retain the marker.
Records: `[3d] probe phase=identity|preflight|armed`, `[3d] fullsize`,
`[3d] precision`, `[3d] combiners`, `[3d] probe driver=vmwaresvga result=`.

## 10. Admission and capability snapshot

`qualified()` = Q1 and Q2 passed in this boot, owner granted Q4, live
context, same generation, healthy engine. Snapshot: 64/3-vertex minimum until
Q2; then mode size, 768 vertices, `max_pixel_work = 4*w*h`,
`CIUKI_R3D_AFFINE_EXACT`; after Q3 also `CIUKI_R3D_PROJECTIVE_BILINEAR`,
texture size min(256, devcaps) and all blend factors. Units = 2 when
`MAX_TEXTURES>=2`. The backend sets `whole_context_fallback`: a refused AUTO
state retires the context.

## 11. Quake III (H1) and what stays in software

H1 through production GL/GDI/wire/service/backend and the host model
(`tests/host/test_vmwaresvga3d_h1.py`): with VMware semantics all 18 measured
frames run on the device (3000 draws one-pass, 4600 two-pass, 2202 prefix,
zero CPU draws); with VirtualBox-GL semantics the prefix (sky REPLACE, world,
models) runs on the device and complete frames retire to software at the HUD's
ADD. Software: fog, stencil, D24/D32, polygon offset beyond the GL frontend's
own handling, Gouraud colour under varying W, textures above the Q3-qualified
size, combiners the host did not qualify.

## 12. Risks

* Host behaviour is an implementation: VirtualBox's DX backend renders
  nothing for VGPU9; its legacy backend ignores combiners; VMware's
  conformance is untested here. Every boot requalifies.
* Modern VMware hosts with guest-backed objects may have dropped or changed
  legacy (non-GB) SVGA3D behaviour; the DEV_CAP path reads their caps, Q1
  decides.
* Per-job surface define/destroy costs host texture creation each job;
  acceptable for correctness, a persistent texture cache is a follow-up.
* Q3 on a slow host may exceed the 30 s budget: perspective refused,
  affine admitted.
* A mode change ends this boot's VMware admission; this family does not
  implement the Savage background requalification hook.

## 13. Lead procedure (VM evidence; VirtualBox use is lead decision G.2)

First inspect the 2D record. On the pinned VirtualBox source the expected
result is `native_fill_unavailable` and software, with no armed 3D probe.
The VGPU9 steps below become reachable only after the lead separately
qualifies a 2D binding for that host. They are a procedure, not VM evidence.

VirtualBox 7.x, VMSVGA with 3D, legacy backend first (VirtualBox 6.x spells
the option `--accelerate3d`):

```
VBoxManage modifyvm "$VM" --graphicscontroller vmsvga --vram 128 --accelerate-3d on
VBoxManage modifyvm "$VM" --uart1 0x3F8 4 --uartmode1 file "$PWD/svga3d-serial.log"
VBoxManage setextradata "$VM" "VBoxInternal/Devices/vga/0/Config/VMSVGA10" 0
```

Add `3d=probe` (and `serial=1`) to `BOOT.CFG`, boot the image once, and keep
the serial log plus `VBox.log`. Expected on the legacy backend:
`[3d] probe phase=identity ... gb_objects=0 hwversion=00020001 ...
texture_ops=00000000`, then `[3d] combiners ... replace0=1 replace1=0 add=0
decal=0` and either `result=qualified` or a clean `result=blocked
reason=minimum_probe` (or `fullsize_probe`) with the expected/actual pixels;
the desktop keeps 2D acceleration in both cases. Repeat after
`VBoxManage setextradata "$VM" "VBoxInternal/Devices/vga/0/Config/VMSVGA10"`
(no value: back to the default DX backend): expected `gb_objects=1` and
`result=blocked reason=minimum_probe`, 2D unaffected. VMware Workstation or
Player (Accelerate 3D graphics on): expected `result=qualified` and all four
combiners. `VBox.log` lines `VMSVGA3d: cap[...]` record the host devcaps for
comparison with the identity record. A `result=failed` line (quarantine) or a
retained `accel-3d-vmwaresvga-15ad-0405.cfg` marker on the next boot is a
finding to report, not to retry blindly.


## Merge6 allocation compatibility

Integration 6 enforces `KHEAP_MAX_REQUEST` in `ciuki/mm.h`. VMware's scratch
service and mode-sized resource shadows exceed the small-object heap bound.
Use the existing Savage owner pattern (`savage3d_owner.c`, `host_alloc` and
`host_free`): page-backed `desktop_buffer_alloc` with `MM_IO_REQUEST`
ownership, an extent/charge prefix, and a combined page-rounded ceiling of
`R3D_RAM_LIMIT` including backend staging. Scratch services, mip staging and
shadows share that accounting; failed allocation rolls its charge back.
No memory limit changes. The owner fixture enforces the heap bound and tracks
the page allocation sizes, owners, live bytes and failure cleanup.

Integration 6 also adds shared stencil flags and precision cancellation hooks.
The VMware precision envelope copy follows the integrated Savage version.
`VMSVGA3D_FEATURES` explicitly lists the seven already-qualified D16 features;
stencil test/clear must refuse before emission and are absent from capability
snapshots. The differential fixture uses that family subset instead of the
expanded shared feature mask. The encoder continues to disable stencil
(`SVGA3D_RS_STENCILENABLE`, pinned protocol citations in the family header).
