# F7 hardware transport and presentation

Research checked against the repository before implementation:

- [OpenGL 1.1](https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf),
  sections 3.8 and 4: sampling and ordered fragment operations require per-draw
  fallback when quantization or attachment formats cannot be represented.
- [ARB multitexture](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_multitexture.txt)
  refers to the independent stages in OpenGL 1.2.1.
- [Quake III tr_shade.c](https://raw.githubusercontent.com/id-Software/Quake-III-Arena/master/code/renderer/tr_shade.c),
  `DrawMultitextured` and `R_DrawElements`: two stages and indexed triangles.
- [Mesa 7.7 savagetris.c](https://chromium.googlesource.com/chromium/deps/mesa/+/3a0db227ffe90888ad760c61a63226988c974e00/src/mesa/drivers/dri/savage/savagetris.c),
  `savageChooseVertexFormat_s4` and `savageFallback`: independently selected
  attributes and fallback. The mirror failed to fetch; checked the pinned local
  source supplied by the lead, lines 1001–1040 and the named fallback function.
- [Linux v6.1 savage_state.c](https://github.com/torvalds/linux/blob/v6.1/drivers/gpu/drm/savage/savage_state.c),
  `savage_dispatch_swap`: copy completion and scanout ownership are distinct.
  The mirror failed to fetch; checked the pinned local source, lines 874–905.
- [X.Org Savage](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c),
  `GetTileAperturePitch`: tiled aperture pitch differs from physical row pitch.
- [SwapBuffers](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-swapbuffers):
  presentation exchanges the window's front and back buffers.

Repository comparison and implementation decisions:

The positive-probe backend currently permits targets up to 64x64 and one
triangle per submission. These are admission limits, not measured silicon
performance. Preserve them. Capabilities describe the admitted mode and format,
including D16, canonical ARGB8888, two stages, texture heap and state masks.
D24/S8, RGB565 render targets and polygon offset are unavailable; RGB565 and
ARGB4444 textures remain supported by native texture conversion. Unsupported
OpenGL state or sampling precision falls back before a draw is submitted.

Copied fixed-width records use the existing Win32 multiplexed call. A bounded
staging job is consumed by the existing display watchdog/worker. User addresses
are never dereferenced on that worker. Context/resource/fence ownership is
checked by render3d. Canonical uploads invalidate VRAM residency. Windowed
readback uses the existing GL desktop-surface presentation.

Fullscreen uses an owned linear destination populated by the existing completed
GPU conversion copy. The common presentation service validates the exact lease
and generation at every bounded native write, applies its scale and gamma, and
acknowledges a completed frame only after its final authority check. This path
is GPU conversion followed by CPU scanout upload, not direct GPU scanout or a
flip. Its existing revoke, desktop restore and watchdog remain authoritative.
No new probe qualification, register writes, panic paths or image variants.

The encoder admits constant exactly representable UV/Q and Q/W on both stages.
That proves zero derivatives: MAG sampling and level zero apply even when MIN
selects mipmaps. Nearest/bilinear MAG and repeat/clamp-to-edge are mapped through
the advertised masks. Varying coordinates remain fallback. The merged f7-03
fraction-preserving recovery makes the old 8x8 minimum-test example agree;
the replacement 3x3 example uses U=32770/32767/32767 and still gives opposite
red/green nearest samples at native pixel (0,6). The test retains its full
255-channel counterexample instead of removing the precision assertion.
