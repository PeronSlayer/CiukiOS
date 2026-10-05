# CiukiOS 0.8.3 screenshots

Actual Linux QEMU/KVM captures from the bounded release runs on 4 October
2026: 800×600 desktop (About updated at 1280×800 after the startup correction), VirtIO GPU 2D with QEMU SDL/OpenGL, one virtual CPU and
256 MiB guest RAM. Captured only the VM client window through the host compositor;
no artwork, screens or responses were generated or substituted.

- `desktop.png`: initial desktop after closing the default About page.
- `about.png`: separate About/Credits tabs and the inverse "Don't show this at
  startup" checkbox; see the [startup/focus verification](../../validation/2026-10-04-about-startup-focus.md).
- `display.png`: detected virtual adapter and monitor, mode list.
- `web.png`: CiukWeb rendering HTTPS with CSS stylesheets, external JavaScript DOM manipulation, and inline PNG, JPEG, and GIF decoded images.
- `google.png`: CiukWeb loading Google over HTTPS (TLS 1.2 with BearSSL), demonstrating HTTP parsing, layout, and image decoding.
- `mouse-scheme.png`: Control Panel Mouse applet showing cursor scheme selection (Tango Default, Classic 95, 3D Contrast) and live cursor preview.
- `window-title.png`: Dynamic window title tab wrapping long document titles seamlessly, bounded before window controls and transitioning into platinum pinstripe rails.
- `doom.png`: a new game using the owner's local DOS Doom installation.

Doom data is not redistributed. These still images do not measure frame pacing,
physical mouse operation, physical ATI/NVIDIA support or Windows execution.
See the [runtime report](../../validation/2026-10-04-release-runtime/README.md).
