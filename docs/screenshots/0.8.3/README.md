# CiukiOS 0.8.3 screenshots

Actual Linux QEMU/KVM captures from the bounded release runs on 4 October
2026: 800×600 desktop (About updated at 640×480 after the layout correction), VirtIO GPU 2D with QEMU SDL/OpenGL, one virtual CPU and
256 MiB guest RAM. Captured only the VM client window through the host compositor;
no artwork, screens or responses were generated or substituted.

- `desktop.png`: initial desktop after closing the default About page.
- `about.png`: corrected About tab and startup preference checkbox; see the
  [post-release verification](../../validation/2026-10-04-post-release-fixes/README.md).
- `display.png`: detected virtual adapter and monitor, mode list.
- `web.png`: real DNS/HTTP response from example.com.
- `doom.png`: a new game using the owner's local DOS Doom installation.

Doom data is not redistributed. These still images do not measure frame pacing,
physical mouse operation, physical ATI/NVIDIA support or Windows execution.
See the [runtime report](../../validation/2026-10-04-release-runtime/README.md).
