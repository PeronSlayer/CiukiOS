# QEMU recording and first web page

## Record a stable window

Build the full image with the verified network and browser payload, then start
the recording profile:

```bash
bash scripts/qemu_record_full.sh --build
```

Without `--build`, the script opens the existing full image. The default client
area is 1280×800 pixels at host position
80,80; the guest's preferred EDID mode is 1024×768. GTK scales guest modes
inside that area. QEMU pauses guest execution until the window reaches the
requested size, so the boot sequence starts at the recording dimensions.
For another recording frame, use `--size WIDTHxHEIGHT` and
optionally `--position X,Y`; `--guest-size WIDTHxHEIGHT` changes the preferred
EDID mode. The host needs QEMU with GTK, KVM, an X11 or XWayland display and
`xdotool`. `--software-display` disables the host OpenGL presentation path.
The window manager may add a title bar outside the requested client area.

The runner presents the same legacy VGA device, NE2000 adapter, SB16/AdLib and
single Pentium III CPU model used by CiukiOS's normal QEMU profile. KVM speeds
CPU execution. GTK OpenGL can help host-side display composition, but it does
not accelerate DOS VGA drawing or expose a DOS 3D driver. For a measured
Doom-vanille experiment in the same fixed window,
`bash scripts/qemu_record_full.sh --vga-fast` selects the legacy VGA TCG path.
On the tested QEMU 11.1 host, that path is not stable
with the locally tested original Doom binary, so the recording profile uses
KVM. The focused Doom-vanille timedemo on the current image took 194 realtics
for 350 gametics with `--vga-fast`, within its 350-realtic gate. This is a
fullscreen, sound-off timedemo, so it does not measure a windowed game or the
original Doom. The frame size and host GL setting do not promise a particular
game FPS.

For a recording, select the QEMU window in OBS or another capture tool. `Ctrl+Alt+G`
releases QEMU's mouse. The selected guest display mode is still editable from
CiukiOS Display settings. QEMU's user network is NAT: the guest normally uses
10.0.2.15, gateway 10.0.2.2 and DNS 10.0.2.3. Outbound Internet access needs
the host to have working network access.

## Browse from CiukiOS

1. From the desktop, open DOS Prompt (F4), run `NETSTART`, then type `EXIT` to
   return to the desktop. Network settings can also request DHCP and start the
   service.
2. Open Application Library → Applications → CiukWeb and select **Go** for
   the initial `http://example.com/` address.
3. Enter another HTTP address in CiukWeb's address field and select Go.
   Up/Down and Page Up/Down scroll; Ctrl+L focuses the address field.
   Absolute HTTP links are clickable in the displayed text.

CiukWeb is a native desktop C module. It uses `C:\NET\HTGET.EXE` and the
packaged mTCP packet driver for HTTP retrieval, then reads and presents the
page in its own window. The desktop temporarily yields to HTGET during a
download. `HTGET` can also fetch a page directly from the DOS prompt:

```text
HTGET -o C:\SHARE\PAGE.HTM http://example.com/
```

CiukWeb supports HTTP text and absolute HTTP links. It does not yet render
images or support HTTPS, CSS and JavaScript. HTTP traffic is unencrypted, so use the first-browse
profile with public pages and no credentials. Some current sites redirect to
HTTPS or require newer browser features; `http://example.com/` is the verified
public demonstration page.

## QEMU verification

`python3 scripts/qemu_test_ciukweb.py --image build/full/ciukios-full.img
--output build/tests/ciukweb-final` uses a disposable disk copy, starts
NE2000 over QEMU user NAT, downloads the public page with HTGET and checks
that CiukWeb renders content in its native desktop window. The earlier
MicroWeb capture in `build/tests/qemu-record-web-20261001/` remains historical
evidence from the previous image and is not the current browser.
The recording-window gate uses a separate disposable image, checks the GTK
client size against the desktop, DOS text and an 800×600 VGA preview, and
records guest framebuffer sizes. All results are QEMU observations; physical
hardware is not covered.

See [QEMU user network documentation](https://www.qemu.org/docs/master/system/devices/net.html),
[QEMU VirtIO GPU documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
for why a guest driver is needed for 3D acceleration, and the
[native app and software OpenGL record](native-apps-and-opengl-2026-10-01.md).
