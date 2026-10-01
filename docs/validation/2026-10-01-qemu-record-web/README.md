# Fixed QEMU window and HTTP browsing — validation

Validation used QEMU 11.1.1 on the development host, KVM acceleration, an
emulated NE2000 ISA NIC, QEMU user-mode NAT and copied FAT16 images. The full
build in `build/tests/qemu-record-web-20261001/final-build3.log` passed.
These observations do not qualify physical hardware.

## Recording window

`python3 scripts/qemu_test_record_window.py` passed. Its report is
`build/tests/qemu-record-web-20261001/record-window/result.json`.

| Guest state | Guest framebuffer | QEMU GTK client area |
| --- | ---: | ---: |
| Desktop | 1024×768 | 1280×800 |
| DOS prompt | 1024×768 | 1280×800 |
| VGA preview | 800×600 | 1280×800 |

The runner used GTK with `gl=on,zoom-to-fit=on`, preferred EDID 1024×768 and
`xdotool` to maintain the XWayland window size. The guest began booting only
after the first resize. Host OpenGL presentation
started successfully. This is not evidence of accelerated VGA or 3D inside
CiukiOS, and it does not establish a new Doom FPS result.

The separate fullscreen Doom-vanille performance gate now enters the DOS
prompt with F4 after desktop startup. On the rebuilt image it passed the
350-gametic timedemo in 194 realtics against a 350-realtic limit. The
sound-off fullscreen result is in
`build/tests/qemu-record-web-20261001/doomvan-perf-test.log`; it does not
measure original Doom or M4 windowed gameplay.
The nested generic taxonomy reports `runtime_stable=FAIL` because its generic
workload rule expects the program to remain open; this timedemo exits to the
shell by design, and the performance wrapper requires that completion marker.

## Network and browser

`python3 scripts/qemu_test_web.py --image build/full/ciukios-full.img
--output build/tests/qemu-record-web-20261001/web-final --browse-wait 40`
passed on the rebuilt image before the final guest README text refresh; that
last rebuild changed no browser or network binary. `NETSTART` installed the packet-driver bridge.
HTGET downloaded 713 bytes from `http://example.com/` through QEMU NAT and
the file contained `Example Domain`. The browser started in an M4 DOS window,
initialized its network interface, stayed running and rendered page text.
The report is `web-final/result.json` in the run directory; the direct QEMU
framebuffer capture is `web-final/browser-page.png`. The README gallery copy,
`docs/screenshots/0.8.0/first-web.png`, has the same SHA-256:
`2123f0f95932be25c5cc8c3f82ba8f0aa19acffe87ef5b1dab02f65ae6dd634d`.

The packaged `MICROWEB.EXE` matches the verified CiukiOS memory build
(`ffcf50d17d1d1bcddcd2aead01468e9daae77321a06a8f0c24108011dc063988`).
`C:\NET\SOURCE\WEB-SRC.ZIP` contains the corresponding modified source and
`C:\NET\BROWSER\COPYING.TXT` contains its GPL notice. The upstream release
binary and source archives are pinned by SHA-256 in
`scripts/fetch_microweb.sh`. The browser supports HTTP and older HTML only;
HTTPS and modern scripted sites remain unsupported.
