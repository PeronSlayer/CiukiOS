# CiukiOS 0.8.3 screenshots (0.8 line, branch `legacy-0.8`)

Actual Linux QEMU/KVM captures from the bounded release runs on 4 October
2026: 1280×800 desktop, VirtIO GPU 2D with QEMU SDL/OpenGL, one virtual CPU and
256 MiB guest RAM. Captured only the VM client window through the host compositor;
no artwork, screens or responses were generated or substituted.

- `desktop.png`: initial desktop after closing the default About page.
- `about.png`: separate About/Credits tabs and the inverse "Don't show this at
  startup" checkbox; see the startup/focus verification.
- `display.png`: detected virtual adapter and monitor, mode list.
- `web.png`: CiukWeb's modern interface (navigation, Reload, Stop, Home, HTTPS badge, address bar, status bar) loading Google over HTTPS with decoded logo and form inputs on the 1280×800 desktop.
- `google.png`: live HTTPS Google session in CiukWeb demonstrating BearSSL TLS 1.2, HTTP parsing, and image decoding.
- `mouse-scheme.png`: Control Panel Mouse applet showing cursor scheme selection (Tango Default, Classic 95, 3D Contrast) and live cursor preview.
- `window-title.png`: Dynamic window title tab wrapping long document titles seamlessly, bounded before window controls and transitioning into platinum pinstripe rails.
- `doom.png`: a new game using the owner's local DOS Doom installation.

Doom data is not redistributed. These still images do not measure frame pacing,
physical mouse operation, physical ATI/NVIDIA support or Windows execution.
See the runtime report.
