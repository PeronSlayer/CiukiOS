CiukiOS software OpenGL prototype

GLDEMO.EXE uses TinyGL 0.4.1, a free software implementation of a
subset of the classic OpenGL API. It draws a coloured triangle in
VGA mode 13h. Press Escape to exit. Open it from Programs/CiukGL
or run C:\PROGRAMS\CIUKGL\GLDEMO.EXE from the desktop.

The OpenWatcom 32-bit DOS/4GW library and GL.H/CIUKGL.H headers
are in C:\SYSTEM\GL. ciukgl_create provides an RGB565 software
framebuffer; an application presents the pixels to its chosen video
surface.
TINYGL.LIB is a static library for CiukiOS DOS/4GW programs. Its
upstream license is in LICENSE.TXT. Build source and reproducible
adaptations are in the CiukiOS repository at scripts/build_opengl.sh.

This is a software renderer and an OpenGL subset. It does not enable
GPU acceleration or Windows OpenGL DLL compatibility.

Native SuperSavage 3D diagnostic

SAV3D.COM uses the native CiukiOS BCI driver on supported S3 hardware.
Press F4 for the system shell, then run SAV3D. It verifies hardware
self-tests, draws a GPU Gouraud triangle, and saves before/after
operation counters to C:\SYSTEM\VIDEO\GPU3D.LOG. Press a key to return.
The system shell retains its checked framebuffer for this client.
An independent DOS-window VM has no access to this system-only API.

The native interface currently provides untextured screen-space
triangles without a depth buffer. It is separate from the TinyGL
software OpenGL library. Unsupported hardware reports that native
triangles are unavailable; it does not report software drawing as GPU
acceleration. Firmware still establishes display timings.
