# CiukiOS 0.8.3 screenshots

Actual Linux QEMU/KVM captures from the bounded release runs on 4 October
2026: 800×600 desktop (About updated at 1280×800 after the startup correction), VirtIO GPU 2D with QEMU SDL/OpenGL, one virtual CPU and
256 MiB guest RAM. Captured only the VM client window through the host compositor;
no artwork, screens or responses were generated or substituted.

- `desktop.png`: initial desktop after closing the default About page.
- `about.png`: separate About/Credits tabs and the inverse "Don't show this at
  startup" checkbox; see the [startup/focus verification](../../validation/2026-10-04-about-startup-focus.md).
- `mouse-scheme.png`: Control Panel Mouse applet showing cursor scheme selection (Tango Default, Classic 95, 3D Contrast) and live cursor preview.
- `window-title.png`: Dynamic window title tab wrapping long document titles seamlessly, bounded before window controls and transitioning into platinum pinstripe rails.
- `display.png`: detected virtual adapter and monitor, mode list.
- `web.png`: earlier text-only DNS/HTTP response from example.com. It does not
  demonstrate the newly implemented HTML subset or PNG/GIF/JPEG image path.
- `doom.png`: a new game using the owner's local DOS Doom installation.

Doom data is not redistributed. These still images do not measure frame pacing,
physical mouse operation, physical ATI/NVIDIA support or Windows execution.
See the [runtime report](../../validation/2026-10-04-release-runtime/README.md).
The web capture predates the HTML/image integration changes. A
replacement 1280×800 CiukWeb capture using local HTML and PNG/GIF/JPEG fixtures
will be added after the integration test; this screenshot set makes no claim
that a public site served or rendered images.
