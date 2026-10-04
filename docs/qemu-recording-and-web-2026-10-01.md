# Record CiukiOS 0.8.3 and open a first web page

This guide uses the full FAT16 image in QEMU. QEMU runs on the host; CiukiOS
is the guest. For build prerequisites, see [Build and run](build-and-run.md).

## Fixed recording window

After building the image:

```bash
bash scripts/qemu_record_full.sh
```

Pass `--build` to fetch the verified optional packages and rebuild first.
Use `--software-display` if host OpenGL presentation is unstable. These
options do not change CiukiOS's guest graphics driver.

| Default | Value |
| --- | --- |
| Host client area | 1280×800 at position 80,80 |
| Preferred guest EDID mode | 1024×768 |
| CPU and display | One Pentium III model, KVM and standard VGA |
| Network and audio | QEMU user NAT with NE2000; SB16/AdLib |

Use `--size WIDTHxHEIGHT` for another client area, `--position X,Y` to move
it, or `--guest-size WIDTHxHEIGHT` to change the preferred guest mode. The
script waits for the requested window size before letting boot continue.
The window manager's title bar can add pixels outside the client area.
GTK, KVM, X11/XWayland and `xdotool` are needed for this recording mode.

The visual runner supports `QEMU_VIDEO_DEVICE=std` (default), `virtio`,
`virtio-gl` and `ati-rage128`. VirtIO-GPU uses a resource-backed 2D scanout;
`virtio-gl` enables QEMU's host OpenGL renderer for that 2D path. This does
not provide guest 3D acceleration or an OpenGL driver. `ati-rage128` selects
QEMU's emulated Rage 128 Pro for exercising the legacy scanout path. Driver
family recognition and QEMU emulation do not qualify physical ATI/NVIDIA
cards. The Display app can identify the adapter/monitor and preview a guest
mode; confirm the preview to save it.

The optional `--vga-fast` TCG mode is a
Doom-vanille experiment, not the default: on the tested QEMU 11.1 host it was
unstable with the local original Doom binary. A focused sound-off fullscreen
Doom-vanille timedemo took 194 realtics for 350 gametics; that is not a
windowed-game FPS measurement. The final 0.8.3 integration and pacing results
are recorded in the [current project status](project-status-2026-10-04.md).

Select the QEMU window in OBS or another capture tool. `Ctrl+Alt+G` releases
QEMU's mouse grab.

## First page in CiukWeb

1. Wait for the desktop: the detected packet driver starts networking
   automatically. The Network applet can renew DHCP when needed.
2. Open Application Library → Applications → **CiukWeb**. Select **Go** to
   fetch its initial `http://example.com/` page.
3. Enter another HTTP URL in the address field. Ctrl+L focuses it; Up/Down
   and Page Up/Down or the wheel scroll. Follow links or use Back/Forward,
   Reload (Ctrl+R) and Stop.

CiukWeb's window and HTML text view are native C desktop code. The bundled
native cooperative HTTP client uses the resident packet bridge and keeps the
desktop visible. The separate mTCP `HTGET` utility remains available from DOS:

```text
HTGET -o C:\SHARE\PAGE.HTM http://example.com/
```

| Works now | Still open |
| --- | --- |
| Native DNS/HTTP, text, links, navigation history and reload. | HTTPS, images, CSS and JavaScript. |

HTTP is unencrypted; use public pages without credentials. Sites that force
HTTPS or require scripts may not display. QEMU user NAT normally gives the
guest 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3; outbound Internet
access also needs a working host connection.

## Evidence and further reading

The [0.8.3 runtime gate](validation/2026-10-04-release-runtime/README.md) resolved,
fetched and reloaded `http://example.com/` on a snapshot QEMU image. The earlier recording gate
checked the fixed client area across desktop, DOS text and VGA preview modes.
The earlier MicroWeb capture is historical; MicroWeb is no longer installed.
These are QEMU observations, not physical-PC results.

See the [native app and OpenGL boundary](native-apps-and-opengl-2026-10-01.md)
and the [network settings guide](network-settings-2026-10-01.md).
