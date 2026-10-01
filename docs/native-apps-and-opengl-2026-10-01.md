# Native applications and software OpenGL

**Chosen language: C with [OpenWatcom 2.0](https://open-watcom.github.io/open-watcom-v2-wikidocs/guitools.html).**
NASM remains the choice for the boot path, kernel interfaces and VM assembly.
CiukPaint, CiukNote, Files and CiukWeb already share the native desktop ABI
in `src/apps/app.h`, `ui.c` and `rt.c`.

## Which executable should a developer build?

| Format | Use today | Limit |
| --- | --- | --- |
| `.APP` (`CAPP`) | Native C desktop window module, called by `SHELL.COM`. | Registered during the system build; one segment with code, data and 4 KiB stack under 64 KiB. |
| DOS `.COM` / `.EXE` | Console tools, legacy games or 32-bit OpenWatcom DOS/4GW code in an M4 window. | Uses the DOS process ABI and virtual VGA, not the desktop `.APP` API. |
| `.LIB` | Static code linked into a DOS/4GW executable, such as `TINYGL.LIB`. | No Windows DLL or general dynamic library loader. |
| PE32 Windows `.EXE` | Header is recognized and shown as unsupported. | No PE loader or Win32 APIs; Windows 95/98 apps and installers cannot run. |

A source application can join the desktop by adding it to
`scripts/build_apps.sh` and the shell module table. The next SDK step is a
stable installable `.APP` manifest and loader, so third-party apps no longer
need those system edits.

## CiukWeb: native window, first HTTP transport

`BROWSER.APP` provides CiukWeb's address field, scrolling and HTML text/link
view in native C code. Application Library opens it, and Files routes `.HTM`
and `.HTML` to it. MicroWeb is not installed in the current image.

| Works now | Missing |
| --- | --- |
| Simple HTTP text and absolute HTTP links; QEMU fetched `http://example.com/` through an NE2000 and showed it in the native window. | HTTPS, CSS, JavaScript, images, native TCP socket service, history and cache. |

The first transport runs bundled mTCP `HTGET.EXE`, then CiukWeb reads the
download. The desktop yields briefly during that step. The parser currently
accepts up to 16 KiB, displays 128 lines and tracks 16 links. Larger pages
need streaming and storage outside the module's near-data segment. See the
[first-page guide](qemu-recording-and-web-2026-10-01.md).

## TinyGL: a software graphics starting point

`scripts/build_opengl.sh` fetches SHA-256-pinned
[TinyGL 0.4.1](https://bellard.org/TinyGL/) source, applies mechanical
OpenWatcom syntax changes and builds a 32-bit DOS/4GW static library.
The image includes `C:\SYSTEM\GL\TINYGL.LIB`, `GL.H`, `CIUKGL.H`, the
upstream license and `C:\PROGRAMS\CiukGL\GLDEMO.EXE`.

`ciukgl_create()` gives the demo an RGB565 framebuffer. The demo calls
OpenGL-style matrix, vertex, colour, depth and clear functions, then copies
the software result to VGA mode 13h. In an M4 DOS window, the QEMU gate
checks coloured triangle pixels and Escape exit.

**Boundary:** this is a subset of classic OpenGL, not full OpenGL 1.1
conformance or accelerated 3D. QEMU GTK `gl=on` can accelerate host window
presentation; it does not expose guest GPU acceleration. Wider framebuffers,
a Mesa/OSMesa port and named free-program compatibility tests are future
work, not current capabilities.

## Next steps

1. Publish an installable `.APP` manifest/loader and a sample C application.
2. Move CiukWeb's HTTP transfer into a resident native TCP service; add
   bounded link/history/error tests. HTTPS also needs TLS, certificates and
   trustworthy time.
3. Extend the 32-bit graphics presentation API and test named free workloads
   before advertising a wider OpenGL version.
4. Develop PE32/Win32 separately against the [free probes](windows-compatibility-and-layout-2026-09-30.md).

All results here are from QEMU. Physical 2004-era PCs remain unqualified.
