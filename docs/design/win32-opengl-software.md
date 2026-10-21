# Win32 software OpenGL (f7-07)

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
  format cannot change after selection. Software formats report GENERIC_FORMAT.
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
The existing x87 transform ABI is retained; SIMD and vendor registers are
not used. These facts do not establish complete OpenGL conformance or the
contract's stronger no-FPU requirement. Remaining differences and evidence
are recorded in `build/codex/f7-07-report.md`.

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
`0x10040000`. The fixture reserves that page before creating a window. Seven
scene checksums, frame counts, format/extension counts and glyph-list status
are collected by the kernel from its verified Win32 desktop child. The
selector, loader table, runner model and `f5-core.json` case are registered;
the guest case is authored and unrun.

### Acceptance still open

This is an implementation of the requested main rendering path, not completed
f7-07 acceptance. Some GL 1.1 behavior is still missing or narrower than the
specification: evaluator/feedback/selection paths, user clip planes, texgen,
full pixel transfer/maps/types, independent 1D/2D texture binding, front/back
read/draw-buffer behavior, all display-list validation rules, and full
shared-object deletion semantics. Pixel-format selection currently persists
on the GDI DC, rather than on its owning window across DC reacquisition.
The optional render-size selection still needs application viewport/readback
semantics qualified when display and render dimensions differ. Extension
strings and function lookup are tested for the implemented common 2D path;
complete normative extension conformance is not established. These are
implementation items to finish before claiming the directive's complete
GL 1.1/extension surface.

The inherited transform/lighting path uses scalar floating point/x87 on
PE32, while the rasterizer is integer and SIMD-free. The lead must reconcile
that explicit directive allowance with the stronger no-FPU wording of the
F7 contract. The report records host commands and the sandbox's LeakSanitizer
restriction without disabling leak detection in the new GL suite.
