# Top bar and About refresh — 1 October 2026

This record covers the 0.8.0 development image in QEMU. It does not qualify
physical hardware.

## Build and captures

- `CIUKIOS_VM_WINDOW=1 bash scripts/build_full.sh` passed; build output:
  `build/tests/ui-refresh-verified-build.log`.
- The unedited framebuffer captures are in
  `build/tests/ui-refresh-verified-capture/`. Copies used in the README are in
  `docs/screenshots/0.8.0/` (`desktop.png`, `ciuki-menu.png`, `about.png`,
  `network-settings.png`).
- The desktop capture shows a single aligned strip with five separated
  readings, the approved portrait at upper left, and no wallpaper status
  overlays. The About capture shows the component table, project credit and
  approved dedication without clipping at 1280×800.

## Interaction checks

`build/tests/ui-refresh-top-hits/` contains one QEMU capture after clicking
each top indicator. Sound opened from volume; the IPv4 panel opened from
network; CPU, RAM and disk opened Task Manager's Performance page. The Ciuki
portrait opened the system menu in `ui-refresh-verified-capture/ciuki-menu.png`.
The 640-pixel recovery layout uses shorter labels so the readings leave the
portrait clear; the captured run used 1280×800.

The readings are deliberately specific: RAM is the largest free conventional
DOS block, disk is recent filesystem activity, and the network pulse follows
resident Ciuki packet counters. `VOL --` means a supported AC'97 mixer was not
present in that QEMU configuration.

## DOS-window regression

`python3 scripts/qemu_test_m4.py --image build/full/ciukios-full.img
--output build/tests/ui-refresh-verified-m4-rerun` passed on this build. The
report records DOOM with Files open, bounded close, two DOS VMs, click focus,
keyboard routing and independent close. After the new system-menu path, menu
and Run each responded in 0.73 seconds in that QEMU run. The report and serial
log are in `build/tests/ui-refresh-verified-m4-rerun/`.
