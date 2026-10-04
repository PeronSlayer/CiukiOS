# Native applications and graphics in 0.8.3

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
| Native DNS/HTTP, text and links, Back/Forward history, Reload/Stop and wheel scrolling; example.com and reload pass through NE2000 NAT. | HTTPS, CSS, JavaScript, images, persistent cache and a general socket API. |

The cooperative native transport uses a resident packet bridge; Go and Reload
keep the desktop visible while each poll advances bounded network work. The parser
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

## 0.8.3 display and graphics boundary

The desktop now has a native **Display** settings app for adapter/monitor
identification and a graphical mode preview. A preview must be confirmed
before it is saved; adapter identification and preview do not establish
physical-monitor compatibility. The native mouse wheel scrolls desktop
controls. **About CiukiOS** is the first-start welcome page and can be
reopened from the desktop; `WELCOME.CFG` records whether to show it at boot.

The protected-mode VBE session path has a VirtIO-GPU 2D backend. QEMU's
`virtio` uses a resource-backed 2D framebuffer; `virtio-gl` selects QEMU's
OpenGL host renderer for that same 2D path. It is host-side presentation
acceleration, not a guest 3D API or OpenGL driver. The legacy scanout backend
recognizes bounded ATI Radeon device-ID families and NVIDIA RIVA TNT through
GeForce4/NV25 families, alongside QEMU's standard VGA and VirtIO VGA aliases.
These exact family checks are implemented, but physical ATI/NVIDIA cards have
not been qualified. QEMU's `ati-rage128` profile models a Rage 128 Pro; it is
not evidence for other cards or real hardware.

The 0.8.3 implementation and final runtime gate are tracked separately in the
[current project status](project-status-2026-10-04.md). QEMU visual results do
not qualify physical PCs.

## Next steps

1. Publish an installable `.APP` manifest/loader and a sample C application.
2. Extend the cooperative native HTTP client beyond its checked DNS/HTTP/reload
   path with wider link/history/error coverage. HTTPS needs TLS, certificates and
   trustworthy time.
3. Test named free workloads and real hardware before advertising a wider
   graphics or driver compatibility boundary.
4. Develop PE32/Win32 separately against the [free probes](windows-compatibility-and-layout-2026-09-30.md).

All results here are from QEMU. Physical 2004-era PCs remain unqualified.
