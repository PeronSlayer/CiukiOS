# Record CiukiOS and open a first web page

This guide uses the 0.8.0 FAT16 image in QEMU. QEMU runs on the host; CiukiOS
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

GTK OpenGL can speed **host** composition. It does not accelerate DOS VGA or
provide a guest OpenGL driver. The optional `--vga-fast` TCG mode is a
Doom-vanille experiment, not the default: on the tested QEMU 11.1 host it was
unstable with the local original Doom binary. A focused sound-off fullscreen
Doom-vanille timedemo took 194 realtics for 350 gametics; that is not a
windowed-game FPS measurement.

Select the QEMU window in OBS or another capture tool. `Ctrl+Alt+G` releases
QEMU's mouse grab.

## First page in CiukWeb

1. In CiukiOS, open DOS Prompt with F4, run `NETSTART`, then type `EXIT` to
   return to the desktop. The Network applet can also start DHCP.
2. Open Application Library → Applications → **CiukWeb**. Select **Go** to
   fetch its initial `http://example.com/` page.
3. Enter another HTTP URL in the address field. Ctrl+L focuses it; Up/Down
   and Page Up/Down scroll. Absolute HTTP links in the text are clickable.

CiukWeb's window and HTML text view are native C desktop code. The bundled
mTCP `HTGET.EXE` downloads each page; the desktop yields briefly while it
runs. `HTGET` is also available from DOS:

```text
HTGET -o C:\SHARE\PAGE.HTM http://example.com/
```

| Works now | Still open |
| --- | --- |
| Simple HTTP text and absolute HTTP links. | HTTPS, images, CSS, JavaScript and a native TCP client service. |

HTTP is unencrypted; use public pages without credentials. Sites that force
HTTPS or require scripts may not display. QEMU user NAT normally gives the
guest 10.0.2.15 with gateway 10.0.2.2 and DNS 10.0.2.3; outbound Internet
access also needs a working host connection.

## Evidence and further reading

The [CiukWeb gate](validation/2026-10-01-native-app-gl/README.md) fetched and
rendered `http://example.com/` on a disposable QEMU image. The recording gate
checked the fixed client area across desktop, DOS text and VGA preview modes.
The earlier MicroWeb capture is historical; MicroWeb is no longer installed.
These are QEMU observations, not physical-PC results.

See the [native app and OpenGL boundary](native-apps-and-opengl-2026-10-01.md)
and the [network settings guide](network-settings-2026-10-01.md).
