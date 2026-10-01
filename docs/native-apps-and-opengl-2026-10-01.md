# Native applications and software OpenGL — 1 October 2026

## Language and executable formats

**C with [OpenWatcom 2.0](https://open-watcom.github.io/open-watcom-v2-wikidocs/guitools.html)
is the application language for CiukiOS.** NASM
continues to serve the boot, kernel and low-level VM interfaces. This matches
the existing CiukPaint, CiukNote, Files, Task Manager and Control Panel code,
so new applications can reuse `src/apps/app.h`, `ui.c` and `rt.c` instead of
inventing a second UI ABI.

| Artifact | Current use | Boundary |
| --- | --- | --- |
| `.APP` (`CAPP`) | Native 16-bit desktop window module, built from C, called by `SHELL.COM` through the documented service table. | One segment; code, data and 4 KiB stack must fit 64 KiB. The host currently registers system modules at build time. |
| DOS `.COM` / `.EXE` | Console programs, legacy games and 32-bit OpenWatcom DOS/4GW applications. M4 runs each desktop-launched program in a forked DOS VM. | The DOS/4GW process is native x86 code, but it uses the DOS process ABI and a virtual VGA device; it is not a desktop `.APP`. |
| `.LIB` | Static libraries linked into an OpenWatcom program. The first graphics package is `C:\SYSTEM\GL\TINYGL.LIB`. | It does not provide a Windows DLL or a system-wide dynamic loader. |
| PE32 Win32 `.EXE` | Recognized by the DOS window launcher, with an unsupported-format message. | Loader, DLLs, Win32 APIs and installer/game services are still absent. |

The next SDK step is an installable module manifest and a loader that can
allocate a window and service slot for third-party `.APP` files. Until that
exists, source applications are added to `scripts/build_apps.sh` and to the
shell's module table. A program developer can already target the standard
DOS `.COM`/`.EXE` path without editing the shell; it opens in an M4 window.

## First native browser

`BROWSER.APP` is **CiukWeb**, a desktop-native C module. It owns its window,
address field, scrolling and simple HTML text/link presentation. The first
HTTP transport still calls the bundled mTCP `HTGET.EXE`, then reopens the
download in the native module. It is the default browser in Application
Library, and Files routes `.HTM`/`.HTML` to it. MicroWeb is not installed in
the current image. The QEMU gate fetches the public `http://example.com/`
page through an emulated NE2000 and checks the downloaded bytes and visible
native window. This first version needs a working packet driver and network
service, supports absolute HTTP links, and does not support HTTPS, CSS,
JavaScript, images or a live native TCP socket. The desktop briefly yields
to the DOS fetch utility; the TCP API should move into a resident native
network service before calling this a complete native browser.
The initial parser reads at most 16 KiB of a page and displays up to 128
text lines; larger documents need streamed parsing and storage outside the
16-bit module's near-data segment.

## First OpenGL path

`scripts/build_opengl.sh` downloads the SHA-256-pinned
[TinyGL 0.4.1 source](https://bellard.org/TinyGL/)
from Fabrice Bellard and applies small mechanical OpenWatcom syntax changes.
Its permissive upstream license is installed with the output. The script
builds a 32-bit OpenWatcom DOS/4GW static library, `TINYGL.LIB`, and
`GLDEMO.EXE`. Both are installed in the image with `GL.H`, `CIUKGL.H` and
the license. `ciukgl_create()` supplies an RGB565 framebuffer without
exposing TinyGL's private `zbuffer.h`. The demo calls OpenGL-style
matrix, vertex, colour, depth and clear functions, then copies TinyGL's
software framebuffer to mode 13h. Its triangle is rendered inside an M4
DOS window in QEMU; `scripts/qemu_test_opengl.py` checks coloured pixels and
Escape exit.

This is a **subset of the classic OpenGL API**, not complete OpenGL 1.1
conformance and not accelerated 3D. The chosen renderer is small enough to
bring up the 32-bit DOS route now. A Mesa/OSMesa port, if pursued, requires
a separately verified DOS toolchain build and a presentation interface for
larger framebuffers. QEMU GTK's `gl=on` setting accelerates presentation on
the host computer and is unrelated to this guest software renderer.

## Next native programs

1. Finish CiukWeb's native TCP/HTTP service and add bounded link, history,
   cache and error tests. HTTPS needs a TLS implementation and trustworthy
   certificate/time handling; it is a separate milestone.
2. Expose a stable third-party `.APP` manifest and loader, then ship a sample
   C application with an independent build and QEMU install/launch gate.
3. Extend the 32-bit graphics presentation API beyond VGA mode 13h, and
   measure TinyGL or Mesa compatibility against named free programs before
   advertising a wider OpenGL version.
4. Keep PE32/Win32 compatibility on its own test plan. An `.EXE` suffix alone
   does not make a Windows program runnable.

Only QEMU behavior is qualified here. Physical hardware and performance on
2004-era PCs remain to be measured.
