# OpenGL render3d adapter (f7-09)

Research checked 2026-10-25 before implementation:

- [OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  sections 2.10, 3.8, 4.1–4.3 and 5.5: CPU transformations, independent
  fragment state, ordered framebuffer access and completion before readback.
- [ARB_multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt)
  refers to [OpenGL 1.2.1](https://registry.khronos.org/OpenGL/specs/gl/glspec121.pdf)
  for the normative independent texture stages.
- [EXT_compiled_vertex_array](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_compiled_vertex_array.txt):
  locking does not freeze GL state; transformed data caching is optional.
- [Quake III tr_shade.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_shade.c):
  locked indexed geometry, texture-stage changes and multiple depth/blend passes.
- [Mesa 7.7 savagetris.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c):
  CPU vertex preparation and software fallback on unsupported raster state.
- [Linux v6.1 savage_state.c](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_state.c):
  validated submission and copy-based swap are separate operations.

The Mesa/Linux web mirrors were unavailable in this session. Their named
functions were checked in the lead's read-only pinned source cache instead:
`savagetris.c` SHA-256 `fc5f8666a4d4e18f52ffe48e39e322c341adadbc576c61471304feb9e2ab557b`,
`savage_state.c` SHA-256 `c8bb7d1347327d904d9310a9ff4a524a2ebe64a81f0653ee8fa2528e2a8d855d`.

Repository comparison: f7-02's `render3d.h` is explicitly worker-only,
offscreen, D16/LESS, one nearest/clamp MODULATE stage, with no user syscall.
Hardware admission is currently limited to one triangle and 64×64 targets.
It has neither target upload for mixed rendering nor delegated GPU scanout.
The f7-09 directive owns the user side; these kernel limits are preserved.

The private `sdk/win32/opengl32/backend.h` describes a process-local adapter,
not a syscall, public SDK or a second device service. A future GDI transport
provider may expose the optional `w32_gdi_render3d` getter. That provider must
marshal copied typed render3d requests on the existing display worker, validate
ownership and advertise only qualified features for the requested format/mode.
Absence of the provider retains the software renderer. This dependency remains
an integration decision for the lead and the parallel kernel implementation.

The user backend keeps CPU transformation/clipping unchanged. It snapshots
fragment state and screen-space vertices into bounded batches. An acceptance
callback must check the entire state and sampling semantics before a draw is
admitted. Unsupported state synchronizes and uses the existing scalar path;
it is never approximated by dropping a state bit. Textures/mips are immutable
uploaded versions in a budgeted LRU, retaining owned RAM copies for recovery.

RAM attachments are the last coherent checkpoint. A bounded journal retains
all commands since that checkpoint. Readback uses separate staging storage;
only a successful finish and complete read commits it. A failed submit,
completion or readback replays the journal once into RAM and disables that
session. This also handles partial device execution without duplicate blend
or depth side effects. Evicting pinned textures first establishes a checkpoint.
Mode changes, resize, drawable rebinding and destruction drain or recover
before attachment/resource lifetime ends. Flush submits; Finish synchronizes.

`minimum.c` translates the existing native subset with explicit Y inversion,
Q4 to Q16 coordinates, D16 and Q20 to Q16 projective coordinates. Its independent
scalar fixture requires identical sample coverage, at most two channel levels
and one D16 level of difference. It rejects other state, including LEQUAL, blend, offset,
second-stage sampling, mip filters and unsupported target formats. This encoder
is available to the eventual provider; it does not add missing target uploads,
wire transport or state to the existing kernel. Provider texture allocation
remains authoritative, including layout padding and heap fragmentation; the
user LRU's canonical-byte budget never promises a successful VRAM allocation.

GPU presentation requires the existing exact lease token/generation and a
provider's completed authorized copy; an unsupported copy uses coherent CPU
presentation. No new lease, raw scanout access or successful GPU-present claim
is inferred from offscreen rendering. The current kernel cannot provide this
operation; the host provider tests the user boundary only.

Host models are evidence for this adapter, its state snapshots and ordering.
They do not qualify the missing kernel transport, broaden hardware admission,
or establish silicon precision, performance, probe qualification or scanout.

Lead amendment 1 research checked 2026-10-26:
[SwapBuffers](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-swapbuffers)
operates on the window's front/back buffers independently of GL draw selection.
[OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
sections 3.8.1, 4.2.1, 4.3.2 and 5.5, defines nearest texel selection,
independent draw/read selection and ordered completion. Repository inspection
found that GPU presentation used the current loaded draw attachment, and swap's
CPU back-to-front copy did not invalidate a previously loaded front attachment.
Restrict GPU swap to the loaded back attachment, synchronize other targets for
CPU presentation, and invalidate coherence before copying back to front.

The software plane interpolator truncates while the native scalar interpolator
rounds. For U values 32767/32771/32771 on the triangle (0,0)/(8,0)/(0,8),
the first sample selects opposite columns of a two-column texture. A coordinate
error below one Q16 unit can therefore cause a full-channel error. Until native
sampling equivalence is established, the minimum encoder admits textured
triangles only with constant UV and reciprocal-W and lossless Q20/Q16 conversion
and native premultiplication. All other textured triangles fall back per draw;
the broad adapter's capability contract is unchanged. This conservative subset
avoids a per-pixel admission scan and retains exact constant-coordinate draws.
