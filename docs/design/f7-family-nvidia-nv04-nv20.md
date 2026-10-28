# F7 NVIDIA NV04–NV20: software-only admission

Date: 2026-10-28. Applies to f7-26, all its lead notes, D-F1–D-F7 and the
owner's explicit software-only exception.

**Decision: software 3D only.** The descriptor is `R3D_SOFTWARE_ONLY`, with
no backend provider. A complete source-backed PIO 3D/context lifecycle on
the existing owner has not been established. This is an implementation
limit, not a claim that the chips lack 3D or that PIO is impossible. NV03
Riva 128 stays on its separate software path.

The incomplete initial draft's working encoder, fake-engine qualification,
precision measurements and positive boot probes were unsupported. This
record supersedes those assertions. Its speculative object constants,
transport-arming flags and weak activation hooks are removed.

## Primary sources

Read-only roots supplied by the lead:

- `L`: `/home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/linux-6.1-drm/drivers/gpu/drm/nouveau/`.
  Linux v6.1, checkout `830b3c68c1fb1e9176028d02ef86f3cf76aa2476`.
  [Pinned upstream tree](https://github.com/torvalds/linux/tree/v6.1/drivers/gpu/drm/nouveau).
- `M`: `/home/peronslayer/Desktop/CiukiOS/build/downloads/f7-sources/mesa-7.11.2/Mesa-7.11.2/src/mesa/drivers/dri/nouveau/`.
  Mesa 7.11.2 nouveau_vieux, from the
  [primary release archive](https://archive.mesa3d.org/older-versions/7.x/7.11.2/MesaLib-7.11.2.tar.bz2).
- `X`: the integrated X.Org port in `src/kernel/drivers/video/nv2d/`,
  pin `0b6619fec39fada995646fb95bb25e72d3c9a491`;
  [nv_hw.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_hw.c).
  `hardware.c:nv2d_setup`, `engine.c:nv2d_emit/nv2d_idle` and `pio_table.h`
  establish the existing 2D owner, not a 3D initialization recipe.

The Mesa release index was also checked online. The v6.1 raw GitHub fetch
was unavailable to the web tool; code evidence uses the pinned local tree.
No new downloads or unpinned envytools source are used; Mesa already contains
generated rnndb headers. The earlier draft's linux-6.1-nouveau and envytools
hashes are not relied on. Every cited source file is hashed below.

The vendor pack was inspected read-only, including the ForceWare 81.98
installer Readme: supported products, component inventory and installation
instructions, not a PFIFO/PGRAPH programming specification. It also contains
binary control-panel help. No vendor files are copied or redistributed.
Windows 98 Resource Kit `wrkc30_3.htm:426-436` describes OS-owned AGP/GART
setup and capability/heap publication, not ownership inferred from a mapped
aperture. No GART mapping, RAM reservation or memory limit changes here.

## Exact candidates in the 2D table

Vendor `10de`. These are literal IDs in `nv2d/matches.h`, not inclusive
ranges. None receives hardware 3D admission.

| Generation / candidate boards | PCI IDs | Mesa class selection source |
| --- | --- | --- |
| NV04/NV05 TNT, TNT2, M64, Vanta | 0020, 0028, 0029, 002a, 002c, 002d | `M/nv04_context.c:204-215` |
| NV10 GeForce 256 | 0100, 0101, 0103 | `M/nv10_context.c:459-469` |
| NV11 GeForce2 MX | 0110, 0111, 0112, 0113 | same |
| NV15 GeForce2 GTS/Pro/Ti/Ultra | 0150, 0151, 0152, 0153 | same; Mesa selects NV11 class by chipset comparison |
| NV17 GeForce4 MX | 0170, 0171, 0172, 0173, 0174, 0175, 0176, 0177, 0178, 0179, 017a, 017c, 017d | same, NV17 class |
| NV18 GeForce4 MX | 0181, 0182, 0183, 0185, 0186, 0187, 0188, 018a, 018b, 018c, 018d | same, NV17 class |
| NV20 GeForce3 | 0200, 0201, 0202, 0203 | `M/nv20_context.c:472-478` |
| NV25 GeForce4 Ti | 0250, 0251, 0253, 0258, 0259, 025b | same, NV25 class |
| NV28 GeForce4 Ti | 0280, 0281, 0282, 0286, 0288, 0289, 028c | same, NV25 class |

UMA IDs 00a0/01a0/01f0 and the broader table's NV3x/NV4x/bridge devices
retain their existing 2D rules and software 3D. The table has 168 IDs.
Family-wide potential metadata (4 units, 4096 texture size) covers that
broader table; it is never an application capability for these chips.

## Observed features versus exposed capabilities

These are source observations, not implemented hardware capabilities.

| Feature | NV04 | NV10–NV18 | NV20/NV25/NV28 |
| --- | --- | --- | --- |
| Triangle/vertices | CPU T&L, screen x/y/z, reciprocal W, packed diffuse/specular, two UV pairs; eight 40-byte slots (`M/nv04_3d.xml.h:681-719`, `M/nv04_render.c:120-165`) | Array/immediate vertices, ring BEGIN/END (`M/nv10_render.c:156-167`) | Array/immediate vertices, ring BEGIN/END (`M/nv20_render.c:179-216`) |
| Texture units | 2 (`M/nv04_driver.h:38`) | 2 (`M/nv10_driver.h:35`) | 4 (`M/nv20_driver.h:35`) |
| Texture formats | Packed RGB(A), including RGB565 and ARGB8888 (`M/nv04_3d.xml.h:393-401`) | Luminance/intensity, packed RGB(A), DXT (`M/nv10_3d.xml.h:862-878`) | Same plus rectangle/depth formats (`M/nv20_3d.xml.h:1238-1262`) |
| Maximum texture | 1024, 11 levels (`M/nv04_context.c:191`) | 2048, 12 levels (`M/nv10_context.c:445`) | No explicit MaxTextureLevels assignment at `M/nv20_context.c:458-464`; no maximum inferred from that file |
| Target/depth/stencil | RGB565 / ARGB8888 surfaces, packed depth/stencil controls (`M/nv04_3d.xml.h:95-112,619-670`) | RGB565 / ARGB8888, Z16 / Z24S8 (`M/nv10_3d.xml.h:127-137`) | RGB565 / ARGB8888, Z16 / Z24S8 (`M/nv20_3d.xml.h:129-139`) |
| Blend/alpha/depth/shade/fog | `M/nv04_state_raster.c:100-200,257-328` | `M/nv10_state_raster.c:35-178`; context defaults | `M/nv20_context.c:249-306,365-366`; defaults |
| Multitexture/mips | Two-stage filters/combiners (`M/nv04_3d.xml.h:422-447`) | Two-stage filters/register combiners (`M/nv10_3d.xml.h:921-941`) | Four-unit filters/combiners (`M/nv20_3d.xml.h:1318-1354,1979-1981`) |

**Exposed hardware features: none.** All formats, texture units, filtering,
blending, alpha, depth/stencil, fog and perspective use existing software
semantics. No new swizzle, IEEE conversion, depth packing or guessed combiner
encoding is retained in production.

## Transport/context evidence and the blocking gap

1. `M/nouveau_context.c:131-141` allocates a hardware channel and fails if
   allocation fails. Graphics objects are allocated on it in
   `nv04_context.c:204-215`, `nv10_context.c:459-469` and
   `nv20_context.c:472-478`. Initialization binds notifier, VRAM and GART
   objects (`nv04_context.c:118-135`, `nv10_context.c:205-214`,
   `nv20_context.c:104-117`). Even immediate vertices use the ring; they do
   not bypass this channel/object lifecycle.
2. Linux v6.1 FIFO class lists expose DMA channel constructors, not a PIO
   3D channel constructor: `L/nvkm/engine/fifo/nv04.c:380-392`,
   `nv10.c:42-54`, `nv17.c:79-92`. NV20/NV25 also select that FIFO
   (`L/nvkm/engine/device/base.c:271-310`).
   `L/nvkm/engine/fifo/dmanv04.c:165-225` requires a push buffer and sets
   RAMFC GET/PUT/fetch state; its object constructor installs RAMHT entries
   (`:47-69`). No equivalent complete PIO 3D constructor is established here.
3. NV04 requires software-emulated binding and surface clipping, not only
   object handles (`L/nvkm/engine/gr/nv04.c:399-445,523-556`). Its interrupt
   path processes trapped methods/context switches and restarts the graph
   FIFO (`:1272-1328`). Allocating the object (`:1043-1060`) alone does not
   supply this. CiukiOS's 2D owner has no equivalent implementation. Guessed
   prefilled instance words cannot replace it.
4. NV10 has hidden vertex-buffer state that Linux explicitly restores
   through FIFO injection because MMIO cannot restore it
   (`L/nvkm/engine/gr/nv10.c:814-906`), plus register/pipe save/restore
   (`:909-939`) and initialized channel pipe state (`:1001-1045`). NV17
   adds context. Rebinding subchannels and 2D pitch does not establish this.
5. NV20 allocates a 0x37f0-byte instance context and registers it in a
   PGRAPH channel table (`L/nvkm/engine/gr/nv20.c:17-26,75-100,220-237`).
   NV25 uses a distinct 0x3724-byte layout (`L/nvkm/engine/gr/nv25.c:21-42`).
   The X-derived 2D owner does not establish these context images. Mesa's
   user-state initialization presupposes the kernel context.

Compared with those sources, CiukiOS `nv2d_setup` installs only its 2D
objects/PIO state; `nv2d_emit/nv2d_idle` prove only bounded 2D transport.
The adapter has no 3D context owner or positive 3D heap reservation producer.
The inspected sources do not prove a complete 3D path and restoration on
that existing owner without the missing channel/context machinery. A boot
experiment cannot substitute for that prerequisite proof.

**DMA is not synonymous with bus mastering.** A DMA surface/texture object
can target local VRAM: `L/nvkm/engine/dma/usernv04.c:104-117` distinguishes
VRAM, PCI and PCI_NOSNOOP. A local-VRAM command ring also differs from
host-memory fetch. This research does not prove host DMA is inherently
required by the chips. It establishes the missing complete PIO/context and
restoration proof for this implementation. Per the owner's exception it
stays software-only; no bus-master bit, host DMA, new channel, GART or
interrupt handler is enabled.

## Resources, fences, watchdog and restoration

No NVIDIA 3D packets/registers are emitted, so there are no new programming
values to cite or decode. No 3D MMIO reads, VRAM writes, RAMHT/PRAMIN objects,
texture uploads/evictions, engine waits, physical fences or watchdog hooks
occur. The existing 2D lease, cursor, firmware state and stop path remain
intact. The generic software service retains owned RAM resources, immutable
texture shadows, bounded work, typed validation and software fences.
AUTO/SOFTWARE contexts stay software for their lifetime; HARDWARE_REQUIRED
returns `-EOPNOTSUPP`. Native hardware features/size/texture-heap and the
public snapshot's hardware fields are zero, so opengl32 selects software.
The wire snapshot retains its version and display-generation identity.
Potential metadata is not used for admission. No SDK or syscall ABI changes.

D-F1 does not apply: no hardware attachment, new RGB565 tolerance or
destination-alpha exception. D-F2 does not waive PGRAPH context/ownership:
no 3D shadow baseline was established and 2D does touch PGRAPH. Every 3D
path restores trivially because it never enters the engine. D-F3–D-F5
concern other families. Tests follow D-F6; hashes below follow D-F7.

## Boot qualification, precision, Quake III and risks

`3d=probe` is inert for this family. No minimum/full-size/precision probe or
background qualification worker is installed. No 3D marker is written or
cleared and no 3D attempt can quarantine working 2D. Safe mode, accel=0,
inactive/active 2D and newer matched devices all retain software 3D.

Quake III needs ordered clears/copies/presentation, full-size color/D16,
flat/Gouraud/textured triangles, all admitted depth/alpha/blend combinations,
two stages, mipmapped repeat/clamp and perspective. All remain software.
GPU counts must stay zero; CPU work must be reported as CPU work.

No hardware precision envelope, throughput or silicon compatibility is
asserted. The removed draft's encoding/tolerance claims were unproved: for
example NV20 encodes Z24S8 as 0x20, whereas NV10 encodes it as zero
(`M/nv20_3d.xml.h:129-130`, `M/nv10_3d.xml.h:127-128`). Future work must
establish depth semantics, coverage/sample convention, joint filter/LOD
witnesses and whole-context fallback before advertising caps. A fake model
cannot prove unknown context state or physical idle safety. Host scalar
fallback tests cannot establish hardware precision or performance.

## Owner hardware procedure and remaining acceptance gates

On a canonical image built by the lead:

1. Capture image SHA-256, PCI ID/revision, BARs, VRAM, mode, serial/disk logs.
   Check existing 2D qualification independently.
2. AUTO must render in software with zero hardware snapshots;
   HARDWARE_REQUIRED must fail. Repeat with 3d=probe and safe mode: no NVIDIA
   3D qualification-success log or 3D attempt marker should appear.
3. Run native scenes and Quake III; record CPU/GPU counts and normal
   presentation/mode restoration. This establishes software compatibility only.

Hardware 3D still needs a reviewed source-backed PIO/context lifecycle (or
separately authorized transport), explicit disjoint heap reservations,
restoration on every success/refusal/timeout/fault and post-probe 2D
self-check. Then implement validator/emitter, an independent packet model,
fuzz/security/differential coverage, minimum/full-size/precision gates on the
budgeted owner path, and an H1 replay with hardware/CPU counts. The existing
revision provides no executable hardware qualification procedure.

No owner decision is needed to retain this authorized software-only result.
Hardware acceleration remains unimplemented; its acceptance criteria remain
open and must not be marked qualified.

## SHA-256 inventory

Paths use `L` and `M` above. Hashes identify inspected bytes, not silicon evidence.

| Source file | SHA-256 |
| --- | --- |
| `L/nvkm/engine/device/base.c` | `3fb2a74ef79d82d43ed2a6130d01e0392569953cd282e56c56d63b288278c983` |
| `L/nvkm/engine/fifo/nv04.c` | `e324da04cf632dcdfcbccd0f13e0bb50ebdd2adfc8047f6d557910d047cf04d2` |
| `L/nvkm/engine/fifo/nv10.c` | `32953ff036f35ec39aeb766740085fa6323a1b0517e04429099c1d18356d8dda` |
| `L/nvkm/engine/fifo/nv17.c` | `aa84afbab259c8dca0975b165552b3d8f82c6a8b2c4c0ce25cae02ad1771b8ec` |
| `L/nvkm/engine/fifo/dmanv04.c` | `3cd01e8021cdf74d5db68950c21e97345667bef6b6893e5b5889b0f4f1d74deb` |
| `L/nvkm/engine/gr/nv04.c` | `1835bc4e6ff1471f075f33c2a355cd325c327a0cf9dab4ca4436b03dd8f4df10` |
| `L/nvkm/engine/gr/nv10.c` | `856791261765e1c9b21a1d2c81dde86daf625e7dfed5cfaa5eb63060cb544439` |
| `L/nvkm/engine/gr/nv20.c` | `173121ee67a08c775e95d52b7670fd45232b6df3f329cea86273b6c198f23f34` |
| `L/nvkm/engine/gr/nv25.c` | `3db16fcce45109bec99c82922078e115e315d27480bbe811dafbca14a0e76061` |
| `L/nvkm/engine/dma/usernv04.c` | `9a688dd7734062617fed3857545abe61b24661714604ba56ff0677d7329c067d` |
| `M/nouveau_context.c` | `109d028797e18a41866d19db4b68f4e7c875668ba782cb556beaba027346c158` |
| `M/nv04_context.c` | `e2d7bb1b5938bfebae25be1fad1b7b8ff74a0915434fb7d6a89cbc8829354a7b` |
| `M/nv10_context.c` | `3cdc3e4142b32e9db7c958887f92a46e8b103becafe377e1a83ec962238b239f` |
| `M/nv20_context.c` | `006c3ddd51bc851ce686abaf4ba72399fbe2188e0e3343786a7de288355c7faa` |
| `M/nv04_driver.h` | `5e168ec916488bf436b1af45fcb7de31ea444bd9c4012bbfc8c3a32c8e590ccb` |
| `M/nv10_driver.h` | `146666aa5a6266532fdbbbadedc5d04d7195d537117f66f848b694eb6f8dc72b` |
| `M/nv20_driver.h` | `d08ba1e0e7ccafb16b9dba259a7db054061b94eaeaf9b3862e2e2ff74281c6f0` |
| `M/nv04_render.c` | `289d390af257b45255f897ae7e13d278427a6c663321d840e785122bc30ad9e4` |
| `M/nv10_render.c` | `5692e6c6974ad5ad579ec21e5f32bb8850065ed9b566643c5103355aa57e6e67` |
| `M/nv20_render.c` | `51fb138df56e83f5ac2a7c6864ac7334288af9f060cb8f71fdc7c4c4c911a4cd` |
| `M/nv04_3d.xml.h` | `52bdf27ba7937277cf353fc1ed9edf544eedca6da12b8ec61eaaa8b4fd1a78ff` |
| `M/nv10_3d.xml.h` | `032f1427ae2a572ff958569c216af2ecb47cdc8f6fcafc387a47265aa07501b4` |
| `M/nv20_3d.xml.h` | `7be21f77261e554ee0dc435f819102dffdd00a93a82e2242a7d247549915ff35` |
| `M/nv04_state_raster.c` | `1ce816f5ab1cfccc75a99b715b0d37eab0b11afc504844141a4c1bedce27321a` |
| `M/nv10_state_raster.c` | `580332184fecb20ffae15e6e1ec1a4030697ad8a4227b70462353f156850e270` |
| `MesaLib-7.11.2.tar.bz2` | `57746aa8c60bc6f5596ec6c6c7274de40fa12edce5ec33fe899e704fa00b5357` |
| `wrkc30_3.htm` | `51c1d5d87de5321d2ecb04ca0f994f4efc98836bdfc85c580c109c3e674f5a40` |
| `X/hardware.c` | `69d1a732b0f0d90a6972b3e041c8568a7c8f5fb8d2023a9f8d6a362d6bd16556` |
| `X/pio_table.h` | `f3b705a5df6f18cc9957983f6c107febe24b93415406211d99d418e65172ec60` |
| `X/README.md` | `90001afb69024937550a782549fbe5d72c18a2a238224b5f42f4a6c4c39203b9` |
| `X/matches.h` | `643870cd37fb987781bb65f9e7a1ab38b92468e94773fb94d99c3f70e47772b3` |
| `X/engine.c` | `287313ae5052edb1a883b64ea48442b32a6ec1eac3c049aa3ac5b0d0cb3cd5db` |
| Vendor installer `/Readme.txt` (read only, not reproduced) | `7b3769c07ff3ec371c51411b6bae2b2905aec105fd0c017775e7a07c9f9d683e` |
