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
