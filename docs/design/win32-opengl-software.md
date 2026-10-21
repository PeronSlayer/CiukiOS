# Win32 software OpenGL (f7-07)

## Primary sources and repository comparison

Research, 2026-10-20:

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
and list machinery, with an opt-in triangle callback. Its fragment backend
uses bounded integer edge/interpolation loops, 16-bit depth and XRGB colour.
The existing x87 transform ABI is retained; SIMD and vendor registers are
not used. These facts do not establish complete OpenGL conformance or the
contract's stronger no-FPU requirement. Remaining differences and evidence
are recorded in `build/codex/f7-07-report.md`.
