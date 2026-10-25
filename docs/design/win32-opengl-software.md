# Win32 software OpenGL (f7-07)

## Quake III Arena (f7-08): research and implementation decisions

Primary sources checked for this amendment:
[id Software renderer and Win32 source](https://github.com/id-Software/Quake-III-Arena/tree/master/code)
(`qgl.h`, `win_qgl.c`, `win_glimp.c`, `tr_init.c`, `tr_backend.c`,
`tr_shade.c`, `tr_image.c`, `tr_surface.c`; also the sky, shadow and view
paths), [GL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
[multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt),
[compiled arrays](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_compiled_vertex_array.txt),
[environment ADD](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_env_add.txt),
[S3TC](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt),
[legacy S3 tokens](https://registry.khronos.org/OpenGL/extensions/S3/S3_s3tc.txt), and
[WGL swap control](https://github.com/KhronosGroup/OpenGL-Registry/blob/main/extensions/EXT/WGL_EXT_swap_control.txt).
The GPL source is a behavior reference; no game code is incorporated into the DLL.
The released GPL renderer is later than the requested 1.11 demo; its inventory
is a source-based compatibility target, pending execution of that binary.

Pre-f7-08 repository comparison: multitexture, ADD, compiled-array lock state and EXT
S3TC decoding already existed. The renderer rejected line width,
stencil operations and stencil clears, and had only 255 live texture slots.
GDI32 owned the four D16/S0 pixel descriptors; new depth/stencil formats required
a coordinated GDI32 change. Quake's normal ICD path calls GDI's format functions
directly, so exporting alternate WGL descriptors cannot fix that path.
Lead amendment 1 selects CiukiOS opengl32 as the system driver, regardless of
backend. Microsoft's [PIXELFORMATDESCRIPTOR reference](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-pixelformatdescriptor)
assigns PFD_GENERIC_FORMAT to the generic GDI implementation; driver-supported
formats clear it. All CiukiOS formats therefore clear PFD_GENERIC_FORMAT and
PFD_GENERIC_ACCELERATED. Quake's default ICD selection accepts them without
`r_allowSoftwareGL`; the renderer string still identifies software rendering.
The same amendment authorizes real GDI descriptors: preserve indices 1–4,
report XRGB storage as cColorBits=32 with RGB 8/8/8 at shifts 16/8/0 and
cAlphaBits=0 (24 meaningful RGB bits; no destination alpha), and add
double-buffered D24/S0, D16/S8 and D24/S8 pairs for XRGB and RGB565.
This cColorBits storage convention is the lead's explicit interoperability
decision; the Microsoft member description otherwise excludes alpha planes.
Keep legacy GL_S3_s3tc absent (Quake uploads its different internal format
tokens); existing EXT block uploads remain independently supported. Swap
control is explicitly accepted as absent for this milestone by amendment 1;
it must stay absent until swaps can actually honor display periods;
the extension requires more than storing an interval in the context.

Performance changes must preserve integer fragment results and all existing
goldens. Use exact incremental interpolation and early rejection before texture
sampling; do not replace perspective correction with an unchecked affine
approximation. Host timings and an explicit scalar PIII cost model are evidence
for this synthesized workload only, not measured guest/game frame rates.


### Quake III coverage inventory

The table covers all 59 GL 1.1 calls referenced by the examined renderer,
view, sky, shadow and Win32 initialization paths. `qgl.h`/`win_qgl.c` additionally
bind the complete GL 1.1 ABI: all 336 declarations resolve in the export census;
that does not imply conformance for unexercised APIs such as evaluators.

| Family | GL 1.1 entry points | Implementation/evidence |
| --- | --- | --- |
| Queries | `glGetString`, `glGetError`, `glGetIntegerv` | Existing paths retained; exact host scenes and Quake default-state/array replay. |
| Immediate geometry | `glBegin`, `glEnd`, `glVertex2f`, `glVertex3f`, `glVertex3fv`, `glColor3f`, `glColor4f`, `glColor4ubv`, `glTexCoord2f`, `glTexCoord2fv` | Existing paths retained; exact host scenes and Quake default-state/array replay. |
| Client arrays | `glArrayElement`, `glDrawElements`, `glVertexPointer`, `glColorPointer`, `glTexCoordPointer`, `glEnableClientState`, `glDisableClientState` | Existing paths retained; exact host scenes and Quake default-state/array replay. |
| Transforms | `glMatrixMode`, `glLoadIdentity`, `glLoadMatrixf`, `glOrtho`, `glPushMatrix`, `glPopMatrix`, `glTranslatef`, `glClipPlane` | Existing paths retained; exact host scenes and Quake default-state/array replay. |
| Raster state | `glEnable`, `glDisable`, `glViewport`, `glScissor`, `glCullFace`, `glPolygonMode`, `glPolygonOffset`, `glShadeModel`, `glLineWidth` | New line-width state and scalar wide lines; existing clipping/culling/viewport paths. |
| Fragment and clear state | `glClear`, `glClearColor`, `glClearDepth`, `glColorMask`, `glDepthFunc`, `glDepthMask`, `glDepthRange`, `glAlphaFunc`, `glBlendFunc`, `glStencilFunc`, `glStencilOp` | New stencil state/operations and rounded alpha reference; D16/D24 attachments. |
| Texture objects and images | `glBindTexture`, `glDeleteTextures`, `glTexImage2D`, `glTexSubImage2D`, `glTexParameterf`, `glTexParameterfv`, `glTexEnvf` | Dynamic names (2048 live objects tested), owned border-color state and GL_CLAMP border filtering; existing uploads/mips. |
| Lists, synchronization and pixels | `glCallList`, `glFinish`, `glDrawBuffer`, `glReadPixels` | Independent front/back read/draw buffers, masked dual clear/write, swap-copy and stencil readback; existing owned lists. |

| Probed extension / WGL calls | Outcome |
| --- | --- |
| `GL_ARB_multitexture`: `glActiveTextureARB`, `glClientActiveTextureARB`, `glMultiTexCoord2fARB` | Advertised, two independent active/client units and matrix stacks; all declared ARB coordinate overloads retain their existing lookup tests. `GL_MAX_ACTIVE_TEXTURES_ARB` is the old name for token `0x84e2` (`GL_MAX_TEXTURE_UNITS_ARB`), returning 2. |
| `GL_EXT_compiled_vertex_array`: `glLockArraysEXT`, `glUnlockArraysEXT` | Advertised; first/count queries, invalid ranges and nested/unmatched lock errors. Array contents remain authoritative; direct validated emission removes repeated dispatch/locking without caching state across draws. |
| `GL_EXT_texture_env_add` | Advertised; saturated RGB sum, base-format-dependent alpha, no new entry points. MODULATE/REPLACE/ADD have specialized scalar loops. |
| `GL_S3_s3tc` | Deliberately absent; the legacy `GL_RGB4_S3TC` request is not accepted. The source disables compression when this extension is absent. |
| `GL_EXT_texture_compression_s3tc` | Existing advertised ARB compressed upload/subimage/retrieval with DXT1 RGB/RGBA, DXT3 and DXT5 decoding remains tested. It does not imply support for the legacy S3 tokens. |
| `WGL_EXT_swap_control`: `wglSwapIntervalEXT`, `wglGetSwapIntervalEXT` | Absent, both lookups return null. Quake explicitly tolerates the failed probe. A display-period boundary and window-owned interval state are required before advertising it; no false success or timer-based imitation is added. |
| `ChoosePixelFormat`, `DescribePixelFormat`, `SetPixelFormat`, `GetPixelFormat` | GDI-owned stable namespace. WGL-named aliases delegate to it for mini-driver clients; normal `opengl32.dll` ICD clients import GDI directly. Indices 1–4 retain D16/S0 and their buffering/color classes; indices 5–10 add double-buffered D24/S0, D16/S8 and D24/S8 in XRGB/RGB565 pairs. XRGB reports cColorBits=32, RGB 8/8/8, alpha 0; all are system-driver formats without GENERIC_FORMAT. |
| `wglCreateContext`, `wglMakeCurrent`, `wglDeleteContext`, `wglGetCurrentContext`, `wglGetCurrentDC`, `wglGetProcAddress`, `wglShareLists` | Existing ownership/rebind/share rules retained. Context attachment precision and double buffering now come from the selected descriptor rather than the numeric format index. |
| `SwapBuffers` / `wglSwapBuffers` | Existing GDI transport / direct WGL alias; successful double-buffer presentation copies back to front and preserves back, matching PFD_SWAP_COPY. Front drawing is submitted by Flush/Finish. |
| `SetDeviceGammaRamp` / optional 3DFX gamma probes | GDI responsibility; no 3DFX gamma extension is advertised here. |

### State, enum and format inventory

| State / data | Enumerants and behavior used by the source |
| --- | --- |
| Queries and diagnostics | `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION`, `GL_EXTENSIONS`, `GL_MAX_TEXTURE_SIZE`, `GL_MAX_ACTIVE_TEXTURES_ARB`; `GL_NO_ERROR`, `GL_INVALID_ENUM`, `GL_INVALID_VALUE`, `GL_INVALID_OPERATION`, `GL_STACK_OVERFLOW`, `GL_STACK_UNDERFLOW`, `GL_OUT_OF_MEMORY`. Renderer/vendor strings remain honest software identification, version starts with 1.1. |
| Geometry / shading | `GL_TRIANGLES`, `GL_TRIANGLE_STRIP`, `GL_QUADS`, `GL_POLYGON`, `GL_LINES`; `GL_FLAT`, `GL_SMOOTH`; `GL_FRONT`, `GL_BACK`, `GL_FRONT_AND_BACK`, `GL_FILL`, `GL_LINE`, `GL_CULL_FACE`, `GL_POLYGON_OFFSET_FILL`; line widths 1 and 3. |
| Matrices / clip / viewport | `GL_MODELVIEW`, `GL_PROJECTION`, `GL_CLIP_PLANE0`; column-major float matrices, user eye plane, viewport/scissor and depth range including collapsed sky and weapon ranges. Raster pixel depth now uses the selected depth precision and depth range too. |
| Arrays and scalar types | `GL_VERTEX_ARRAY`, `GL_COLOR_ARRAY`, `GL_TEXTURE_COORD_ARRAY`; `GL_FLOAT` XYZ at 16-byte stride and ST at 8/16-byte stride, `GL_UNSIGNED_BYTE` RGBA at 4-byte stride; `GL_INDEX_TYPE` is a renderer macro for `GL_UNSIGNED_INT` indices, not a GL enumerant. Both units can change arrays inside a lock. `GL_TRUE`/`GL_FALSE` enable/mask values. |
| Depth, alpha, blending | `GL_DEPTH_TEST`, `GL_ALPHA_TEST`, `GL_BLEND`; `GL_LEQUAL`, `GL_EQUAL`; alpha `GL_GREATER` 0, `GL_LESS` 0.5 and `GL_GEQUAL` 0.5. Reference rounds to nearest byte (0.5 becomes 128), per GL 1.1 sections 2.13.9/4.1.3. Blend sources: `GL_ZERO`, `GL_ONE`, `GL_DST_COLOR`, `GL_ONE_MINUS_DST_COLOR`, `GL_SRC_ALPHA`, `GL_ONE_MINUS_SRC_ALPHA`, `GL_DST_ALPHA`, `GL_ONE_MINUS_DST_ALPHA`, `GL_SRC_ALPHA_SATURATE`; destinations additionally `GL_SRC_COLOR`, `GL_ONE_MINUS_SRC_COLOR`. |
| Stencil / overdraw / shadows | `GL_STENCIL_TEST`, `GL_STENCIL_BUFFER_BIT`, `GL_STENCIL_INDEX`; `GL_ALWAYS`, `GL_NOTEQUAL`, `GL_KEEP`, `GL_INCR`, `GL_DECR` in the game, plus all core comparisons and KEEP/ZERO/REPLACE/INCR/DECR/INVERT implemented and tested. Alpha precedes stencil; stencil fail/depth fail/depth pass operations honor the write mask. Increment/decrement saturate. An absent attachment always passes stencil, clear is harmless and stencil readback reports INVALID_OPERATION. |
| Color and scissor | `GL_COLOR_BUFFER_BIT`, `GL_DEPTH_BUFFER_BIT`, `GL_SCISSOR_TEST`, color/depth write masks. FRONT/BACK/FRONT_AND_BACK and NONE draw selection have separate storage and masked writes. Read selection is independent. |
| Texture state | `GL_TEXTURE_2D`, `GL_TEXTURE0_ARB`, `GL_TEXTURE1_ARB`, `GL_TEXTURE_ENV`, `GL_TEXTURE_ENV_MODE`, `GL_MODULATE`, `GL_REPLACE`, `GL_DECAL`, `GL_ADD`; `GL_TEXTURE_WRAP_S`, `GL_TEXTURE_WRAP_T`, `GL_REPEAT`, `GL_CLAMP`, `GL_TEXTURE_BORDER_COLOR`; `GL_TEXTURE_MIN_FILTER`, `GL_TEXTURE_MAG_FILTER`. |
| Texture filtering | `GL_NEAREST`, `GL_LINEAR`, `GL_NEAREST_MIPMAP_NEAREST`, `GL_LINEAR_MIPMAP_NEAREST`, `GL_NEAREST_MIPMAP_LINEAR`, `GL_LINEAR_MIPMAP_LINEAR`; complete power-of-two mip chains and independent texture units. |
| Image / readback formats | Uploads use `GL_RGBA` / `GL_UNSIGNED_BYTE`; RGB/RGBA internal choices include numeric 3/4, `GL_RGB`, `GL_RGBA`, `GL_RGB5`, `GL_RGB8`, `GL_RGBA4`, `GL_RGBA8`. Screenshots use RGB/RGBA bytes; overdraw uses STENCIL_INDEX bytes. PACK/UNPACK pitch, row length and skips remain tested. The optional legacy `GL_RGB4_S3TC` is excluded as above. |

### Exact scalar optimization and qualification boundaries

The rasterizer intersects three edge inequalities per row, then advances exact
signed quotient/remainder attribute planes across the covered span. Constant
attributes need no increments. Scissor and safe early depth rejection precede
texture work; early rejection is disabled when stencil side effects are active.
A bounded 48-bit reciprocal reconstructs both coordinates exactly, with a
single remainder correction, and can be shared by both units. Very small Q
keeps the original signed division. Trilinear filters keep fractional LOD, sharing
one ordinary-case footprint division among the four quotient-rule derivatives.
Nearest-mip filters calculate only the half-octave decision they consume.
Power-of-two wrap uses masks, bilinear samples reuse row/column addressing,
and common environment/blend modes have direct scalar loops. Amendment 1
also retains exact perspective reciprocals between pixels with identical Q.
Nonmip/nearest-mip decisions compare the undivided footprint numerator against
integer thresholds (257 for minification, 363 for the half octave), with
explicit overflow guards and the original maximum footprint. This eliminates
the ordinary-case footprint division without changing its floor or saturation;
trilinear and large sliver cases retain the old quotient path. Texture-size
logarithms use scalar integer bit scans. Bilinear filtering uses the exact
identity a*(256-f)+b*f = (a<<8)+(b-a)*f without intermediate rounding;
REPEAT/CLAMP_TO_EDGE bypass border checks after their addressing is resolved.

The 1500-case differential traversal shares texture/fragment helpers but uses
the prior bounding-box and barycentric per-pixel division algorithm. It tests
coverage, signed projective reconstruction, six filters, scissor, fog, alpha,
blend, D16/D24 and stencil side effects. Existing independent perspective,
derivative, clipping, pixel-format and fragment oracles remain in place.
A frozen pre-amendment division-based LOD helper independently checks 20000
cases plus exact threshold boundaries, overflow guards and sliver scaling.
Constant non-unity positive/negative Q on each/both units exercises reciprocal
reuse independently against the old per-pixel division traversal.
All seven scene and eleven amendment golden values are unchanged. Old beta-only
error expectations for FRONT/BOTH and stencil state are replaced with legal behavior;
no rendered golden was edited. The four Quake replay goldens were captured
before optimization. Alpha-reference rounding is a specification correction,
covered independently with byte alphas 127 and 128 against GEQUAL 0.5.

`quake3.c` synthesizes 3000 triangles at 320x240 and 640x480: sky (2),
lightmapped world (1600), models (600), alpha/additive effects (400), HUD (398).
The two-pass variant adds 1600 world triangles at EQUAL with DST_COLOR/ZERO.
It uses locked indexed arrays, both texture stages, bilinear mips, ADD,
MODULATE/REPLACE, alpha tests, depth and polygon offset. Frame timings include
command submission, setup, clears and rasterization; they exclude SwapBuffers,
compositor/scanout, game logic and asset loading. Three measured frames follow
a warm frame; readback/checksum occurs outside the measured interval.
D16/S0 remains the comparable baseline; a second replay selects the actual
Quake default XRGB/D24/S8 through production GDI. Both must match the same four
pre-optimization image hashes, with a broad wall-time bound;
a same-process occluded-triangle comparison bounds early-depth regressions.
Profile-mode tests additionally require no ordinary footprint divisions and
fewer perspective divisions than half the covered pixels on this replay.

Host-only SW_PROFILE counters (off in target builds) feed an explicitly
assumed PIII-933 scalar cost model in the fixture, not a host-frequency scale.
[Intel PIII product details](https://www.intel.com/pressroom/kits/quickreffam.htm)
identify the 933 MHz / 133 MHz / 256 KiB L2 class. The cycle coefficients are
engineering assumptions, not Intel latency measurements: triangle 700, span
60, varying-attribute setup 120, attribute increment 12, covered pixel 16,
shaded pixel 25, perspective division 160, LOD 120, wide division 160, texel
12, texture stage 40, fragment 60, destination write 8, submitted vertex 220.
Clear/depth/destination streaming traffic adds a 300 MiB/s budget; cached
texture traffic is charged per texel. The displayed sensitivity range halves
or doubles all costs; it is not a statistical confidence interval. TinyGL's
x87 transform ABI is retained; the target recipe and PE disassembly forbid
MMX/SSE. Host C float code uses the host ABI and is not a PIII timing proxy.

| Indices | Color storage / meaningful RGB | Buffering | Depth / stencil |
| --- | --- | --- | --- |
| 1 / 2 | XRGB32 / RGB565 (24 / 16 RGB bits) | Double | D16 / S0 |
| 3 / 4 | XRGB32 / RGB565 (24 / 16 RGB bits) | Single | D16 / S0 |
| 5 / 6 | XRGB32 / RGB565 (24 / 16 RGB bits) | Double | D24 / S0 |
| 7 / 8 | XRGB32 / RGB565 (24 / 16 RGB bits) | Double | D16 / S8 |
| 9 / 10 | XRGB32 / RGB565 (24 / 16 RGB bits) | Double | D24 / S8 |

ChoosePixelFormat prefers sufficient precision then the smallest excess, in
color/depth/stencil order, with lower-index ties and the original XRGB/D16
zero-request defaults. It respects buffering and requested drawing flags;
unsupported stereo and GDI support fail. Quake's own descriptor enumeration
finds the exact 32/D24/S8 match at index 9. All double-buffer formats report
SWAP_COPY. The fixture/controller and suite capability count are now ten;
scene hashes and the result-page layout are unchanged.

D24/S8 tests use the production GDI namespace and context allocator without a
descriptor adapter. Every format is selected and queried through GDI/WGL;
the 32/D24/S8 default is additionally exercised with allocation-failure cleanup,
masked/scissored clears, stencil readback and ordering, saturation, lists and
raster pixel depth. Stencil is an independent S8 attachment for either D16 or
D24; RGB565 affects color quantization only.
Exact commands, measured speeds and environment failures are recorded in
`build/codex/f7-glperf-report.md`.

## Amendment 3 cross-review decisions (2026-10-21)

Primary sources rechecked before the fixes:
[GL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
§§2.13.7–8, 3.5–3.8 and 4.3 (provoking colors before clipping,
projective S/W, T/W, Q/W interpolation, texture derivatives, bitmap unpack
layout, internal image formats, and pixel coordinates);
[ARB_texture_compression](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_compression.txt)
(the queryable compression hint, initially DONT_CARE);
[wglMakeCurrent](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent)
(exclusive current-thread ownership and compatible drawable rebinding), and
[SetPixelFormat](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setpixelformat)
(window lifetime). Wine's retained declarations remain ABI references only.

The reviewed implementation loses PRESENT geometry before transport, captures
bitmaps with a different pitch from replay, retains an exclusive lease on the
windowed path, and confuses drawable coordinates with the scaled allocation.
Correct those paths and qualify them through production transport/presentation
host adapters. Keep drawable and render sizes separate; application viewport,
scissor, raster and readback coordinates stay in drawable space and map to the
backing allocation. Refresh that allocation on resize and compatible rebinding.
USER's owner-thread transport cannot marshal GL swaps from workers today:
explicitly reject window-context binding from a different thread, including a
released context, rather than let a window DC fall through to generic GDI.
Memory DC contexts retain released-context migration between threads.

Keep texture coordinates and mip gradients wide until wrap/clamp reduction;
interpolate per-unit Q/W and calculate perspective-correct, order-independent
screen derivatives with fractional LOD. Preserve primitive flat color before
splitting/clipping, reuse image-format conversion for subimages, and scope
depth offset to each polygon operation. Guest scene expectations must be
qualified against exact host checksums; dimensions and frame totals must
come from actual allocation and successful completed swaps. Host evidence
does not qualify guest compositor timing or hardware.

The retained fragment representation bounds S/W and T/W to ±2048 (16
fractional bits) and Q/W to ±128 (20 fractional bits), so every stored
interpolant has magnitude at most 2^27. W retains its existing positive
transform bounds. Barycentric accumulation and reconstructed UV use signed
64-bit arithmetic; repeat/clamp reduction precedes any narrowing. Integer
quotient-rule gradients use the maximum absolute screen derivative as the
texture footprint approximation. A bounded binary logarithm retains eight
fractional LOD bits; very large footprints saturate at the largest supported
mip. Identical min/mag filters need no LOD calculation.

`CiukiGLGetSizes` is a value-only diagnostic export of opengl32: six DWORDs
containing render, drawable and physical display width/height pairs for the
current context/DC. It adds no advertised GL extension. The fixture uses it
in the version-2 result block, retaining its 144-byte size and result address.
It requests an exact 320×240 client via AdjustWindowRect, and both the
controller and suite require the seven unchanged, host-qualified checksums.
The guest case remains authored and unrun. Host tests exercise the actual
fixture body (including failed swaps), USER's PRESENT RPC and target query,
du3_request, server receipt, compositor failure/retry, lease restoration and
client teardown. Transport/compositor tests substitute kernel value boundaries.
The fixture-body replay supplies deterministic USER startup, font-success and
process-exit boundaries; production glyph conversion is qualified separately.

## Primary sources and repository comparison

Research, 2026-10-20; primary sources rechecked 2026-10-21:

- [OpenGL 1.1 specification](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  sections 2.5–2.13, 3.6–3.8, 4.1–4.3 and 5.4: errors, transformations,
  lighting, pixel storage, texture environments, fragment ordering and lists.
- [Microsoft rendering contexts](https://learn.microsoft.com/en-us/windows/win32/opengl/rendering-context-functions):
  current contexts belong to threads; a context cannot be current on two threads.
- [PIXELFORMATDESCRIPTOR](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-pixelformatdescriptor)
  and [SetPixelFormat](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-setpixelformat):
  the descriptor is 40 bytes, format indices start at one, and a window's
  format cannot change after selection. Amendment f7-08 above supersedes the
  earlier GENERIC_FORMAT decision: these are system-driver formats.
- [wglGetProcAddress](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglgetprocaddress):
  unknown names and lookup without a current context fail.
- [ARB multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt)
  and [S3TC](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt):
  independent texture units and block upload decoding are software facilities.
- [Wine declarations](https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/opengl32/opengl32.spec)
  (the local Wine 10.0 `include/wine/wgl.h` and `opengl32.spec`): ABI shapes
  only; no Wine renderer implementation is used.

The native TinyGL port has one global context, fixed 256-square RGB textures,
fixed depth comparison, and no mipmaps, alpha test, scissor or standard GL
extension strings. Its native surface adapter and goldens remain a separate
qualification. Win32 needs context ownership, explicit error/capability
reporting and a private back buffer before GDI presentation. New raster state
must not silently change the already qualified native scenes.

The compatibility path keeps TinyGL's CPU transformation, lighting, clipping
with opt-in triangle/line callbacks. Its fragment backend
uses bounded integer edge/interpolation loops, 16-bit depth and XRGB colour.
Amendment 1 permits the scalar x87 transform/lighting ABI in user processes;
the kernel presenter and fragment backend remain integer and SIMD-free.
No vendor registers are accessed. This is a software subset, without a full
GL conformance claim; evidence is recorded in `build/codex/f7-07-report.md`.

## Continuation and implemented boundaries (2026-10-21)

The interrupted WIP did not contain a renderer DLL or a fixture. Its texture
cleanup attempted to free inline TinyGL pixel arrays; those frees were
removed. The retained packed-pixel enum values were also corrected. Vendor
changes are identified and hashed in `sdk/libgl/port.json`.

Additional primary references checked for this implementation:

- [OpenGL 1.2.1](https://registry.khronos.org/OpenGL/specs/gl/glspec121.pdf),
  the normative replacement referenced by the ARB_multitexture registry entry.
- [ARB_texture_env_combine](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_env_combine.txt):
  the six combine operations, RGB/alpha sources and operands, and 1/2/4 scales.
- [ARB_texture_compression](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_compression.txt):
  compressed upload, subimage, size/format queries and retrieval.
- [EXT_compiled_vertex_array](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_compiled_vertex_array.txt):
  the implementation may decline caching; lock/unlock preserve client-array
  behavior and expose their first/count state.
- [EXT_bgra](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_bgra.txt) and
  [EXT_texture_env_add](https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_env_add.txt):
  BGR/BGRA component order and saturated RGB addition with format-dependent alpha.
- [wglUseFontBitmaps](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglusefontbitmapsa):
  each glyph becomes one bitmap display list. GDI's selected font provider
  (including the existing service font bridge) supplies owned coverage; GL
  retains a packed bottom-up copy, bearing and advance.
- [wglShareLists](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglsharelists)
  and [SwapBuffers](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-swapbuffers):
  compatible contexts share objects; swapping has a GDI entry point.

The Win32 adapter retains TinyGL transformation, lighting and homogeneous
clipping. It uses its own bounded, copied display-list commands, rather than
TinyGL lists, so extension state and bitmap ownership follow the same path.
`sdk/libgl/software` supplies integer fragment/depth/blend/texture operations;
the native SDK archive includes this backend without changing the native
adapter's default renderer. It provides XRGB color storage, RGB565
quantization for 16-bit formats, and D16 depth. Four software pixel formats
report zero stencil, accumulation and destination-alpha bits. ABI forwarding
is generated from the retained 336 GL 1.1 declarations and extension shapes;
the export set is not a full GL conformance claim.

Contexts are serialized around TinyGL's selected context; ownership is per
thread. Share groups retain software texture images and copied display-list
commands until their final context is deleted. Two active/client units and
two texture-matrix stacks are distinct. The rendering buffer is a GDI DIB
section. Existing `ddraw-scale` compatibility settings can select 320x240,
400x300, 512x384 or 640x480; size records name the actual render/display sizes.
Fullscreen popup windows obtain the existing USER display lease, submit the
exact token/generation to `W32_DISPLAY_FRAME`, and count only successful
completion. Conversion to RGB565 occurs before a 16-bit lease submission.

For windowed GL, `DESK_V3_PRESENT` has the same bounded 16-byte damage body
as `DESK_DAMAGE`. It requires a nonzero request, window id and generation,
permits no attachments, and allows one outstanding request per window. USER
waits through its existing bounded RPC. The desktop queues its matching
`DESK_V3_RESULT` only after `desk_redraw` completes successfully. Failed
frames retain the request for retry; minimized/collapsed requests fail.
The completion helper has a host test for identity, bounds, attachment
refusal and duplicate completion. Guest timing, revocation and mode-change
qualification remain the lead's work.

The authored fixture and observer share a 144-byte copied result block at
`0x100e0000` (Amendment 2; `0x10040000` belongs to fullscreen). The fixture
reserves that page before creating a window. Seven
scene checksums, frame counts, format/extension counts and glyph-list status
are collected by the kernel from its verified Win32 desktop child. The
selector, loader table, runner model and `f5-core.json` case are registered;
the guest case is authored and unrun.

### Beta boundary

Amendment 1 permits scalar x87 in user-space transforms and lighting; the
fragment backend and kernel presenter remain integer and SIMD-free. The
336 exported GL entry points describe an ABI, not full GL conformance.
Evaluators, selection/feedback, accumulation and stencil operations report
errors. Pixel maps/transfer, non-byte pixel types (except retained RGB565),
texture borders and full front/back draw/read-buffer semantics remain outside
this beta subset; no corresponding extension is advertised. The software
renderer has no hardware acceleration claim.

The v3 completed-present integration review and guest/hardware qualification
remain with the lead. The new GL sanitized suite keeps leak detection enabled;
its host sandbox cannot complete LeakSanitizer's ptrace-based teardown.
The final host commands and precise gate status are in
`build/codex/f7-07-report.md`.

## Amendment 1 implementation decisions (2026-10-21)

The lead permits scalar x87 in user transforms/lighting; the kernel presenter
and integer fragment backend retain their SIMD-free restrictions. The minimal
versioned v3 completion request remains subject to integration review.

Before implementation, the sources above were rechecked: GL 1.1 §§2.10.4,
2.11, 3.8.4 and 5.4 specify transformed eye planes, independent texture targets,
retained bindings in other sharing contexts, and compile-time pointer capture.
Microsoft SetPixelFormat requires window lifetime persistence. The WIP lacked
these paths. Implement six user planes and per-unit texgen, separate bindings
and default objects, reference-counted deleted textures, window-owned format
storage, byte-format pixel oracles and a common display-list validation path.
Evaluators, selection/feedback, accumulation and stencil remain error-returning
beta exclusions. Host tests qualify the advertised extensions; guest evidence
and the sanitized acceptance rerun belong to the lead.

The window-format slot uses a v2 USER/GDI table tail; v1 tables retain their
original prefix and are never read past it. The production USER fixture
checks an exact-size v1 table under ASan as well as the v2 window/DC path.
The final host evidence has seven unchanged fixture goldens and eleven new
amendment goldens, with separate pixel/region/reflection oracles.

## Amendment 2 integration decisions (2026-10-21)

Rechecked the primary GL 1.1 specification (especially §5.5), Microsoft's
SetPixelFormat, PIXELFORMATDESCRIPTOR,
[wglMakeCurrent](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-wglmakecurrent), wglGetProcAddress and
SwapBuffers documentation linked above, and Wine's upstream opengl32.spec
declarations before resolving the merge. The window-owned pixel-format and
per-thread context rules, software format flags and ABI shapes remain as
implemented in Amendment 1. The internal completed-present request is a
CiukiOS transport decision; it does not claim that Microsoft requires display
scanout completion on SwapBuffers. No renderer code is imported from Wine.

Main's shared `GATE_W32_FIXTURE` now launches `gl-exercise` after a successful
registry OPEN and CLOSE, with the same bounded readiness and child-report
authentication as its existing fixtures. The ten core schema IDs and restart
ID are preserved; GL has a separate launcher ID and keeps its own result
protocol. The old GL-specific gate and dialogs launcher mode are removed.
The observer reads `0x100e0000`, leaving main's pages through `0x100d0000`
unchanged, and uses the shared descendant cleanup that stops desktop
supervision before collecting children. All three selector tables retain
main's probes and register `w32-gl` exactly once. The versioned v3
completed-present helper remains linked alongside the shared fixture gate.

The GL suite remains authored and unrun. Host verification commands, results
and any sanitizer-environment failures are recorded under Amendment 2 in
`build/codex/f7-07-report.md`; they do not replace guest evidence or the lead's
integration review.

Canonical-flags compilation of the merged desktop probe also exposed a
4120-byte `restart_link` frame from main's 4096-byte local shortcut buffer.
[Clang's frame-size diagnostic](https://clang.llvm.org/docs/DiagnosticsReference.html#wframe-larger-than)
and [C11 draft N1570 §6.2.4](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf)
were checked. Keep the 4096-byte frame limit and buffer capacity; give this
single-controller probe's scratch buffer static storage duration. The restart
observer already has shared per-run state and does not run concurrently with
another selected probe. This adds 4096 bytes of zero-initialized kernel
storage, without allocation, new failure paths or changes to shortcut parsing.
