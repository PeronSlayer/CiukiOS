# Advanced Network applet — QEMU validation

Image: `build/full/ciukios-full.img` from
`build/tests/network-advanced-compact-build.log` (full build passed).
All runs use copied disk images in QEMU; they do not qualify physical PCs.

The focused gate is:

```sh
python3 scripts/qemu_test_network_advanced.py \
  --image build/full/ciukios-full.img \
  --output build/tests/network-advanced-final-20261001
```

Its `report.json` records a pass for both scenarios:

- **Static settings and adapter navigation.** Control Panel opens the
  network adapter selected in Device Manager and the installed network
  drivers list. An invalid subnet mask and MTU are rejected. Host name `ciukilab` and MTU
  `1400` are then saved in `MTCP.CFG`, including the matching
  `HOSTNAME_ASSIGNED` entry. `mtype` reads the saved values from the test disk.
- **DHCP with a QEMU NE2000 PCI adapter.** The driver pack loads NE2000,
  Network shows PCI ID `10EC:8029`, IRQ 11, I/O `C000`, packet API INT 60h
  and the test MAC `52:54:00:12:34:56`. The DHCP button launches NETCFG,
  NETSTART installs Ciuki's resident service, the mTCP client receives a
  lease, and the desktop returns. The lease is present on the test disk.
  The NE2000 driver is then disabled in the graphical Drivers view; the
  changed `DRIVERS.CFG` persists and a second QEMU boot omits its load.

Screenshots remain unedited in the gate output. Separate QEMU 800×600 and
640×480 captures are in `build/tests/network-advanced-800/` and
`build/tests/network-advanced-640-480/`; the compact dialog keeps all controls
visible at 640×480. The public gallery copies
the adapter and driver views to `docs/screenshots/0.8.0/`.

The preceding build, before the bundled `NET/README.TXT` text update, also passed
`scripts/qemu_test_m4.py` (`build/tests/network-advanced-m4/report.json`)
and `scripts/qemu_test_desktop_polish.py`
(`build/tests/network-advanced-desktop-polish.log`).
The final image passed `scripts/qemu_test_full_network_icmp.sh --no-build`
(`build/tests/network-advanced-final-icmp.log`), including packet-driver load,
static profile persistence, live resident address update and a valid ICMP
Echo Reply.

This verifies packet-driver presence and a working QEMU network path. The
packet-driver API does not expose a uniform cable/link-state bit, so the
applet does not display one. The first detected PCI network adapter is shown
in the Network applet; Device Manager lists the others. Driver enable/disable
is a saved boot choice and takes effect after restart.
