# DOS programs from the desktop, main build — 28 September 2026

Roadmap phase 1 ([roadmap](../../roadmap-dos-vm-desktop-2026-09-28.md)): the
main build starts the VM manager at boot, and the desktop opens DOS programs
in a window by itself (Run, double-click, "Full screen" from the menu).
Windows 3.1 is no longer part of the image.

Reports from `scripts/test_vm_window_profile.sh --image build/full/ciukios-full.img`
(output `build/tests/vm-window-profile-2026-09-28e`), on an image built by a
plain `scripts/build_full.sh` (`CIUKIOS_VM_WINDOW` defaults to 1). QEMU with
KVM, `-cpu pentium3`, AC'97. Nothing here qualifies physical hardware.

The `doom-original`, `doom-vanille-sfx` and `doom-original-sfx` gates are
automatic: the harness opens the game from the desktop with no command typed,
and checks that the VM manager was started at boot
(`vm_started_at_boot`). `dpmi-probe` and `doom-vanille` run through
`\VM\DPMIRUN.COM`. The V86 gates use a copy of the image without VMSTART, as
they load their own monitor.

Earlier runs `...28c` and `...28d` had failures, fixed before this run:
- DOOM's "Game mode indeterminate" (the program's own directory is now the
  current one);
- the window session not ending (`VM_OP_DPMI_RELEASE` after the host unload);
- DPMIRUN's ES clobbered by `INT 2Fh/1687h` in program mode;
- lost quit keys (the harness retries).

## Artifacts under test

`ciukios-full.img` SHA-256 `8cbd2b057926342fa07a1cd4daa34b12de77e482e64f7bfb7f2ba19eb3918804`.

| File | SHA-256 |
| --- | --- |
| `ciukios-full.img` | `8cbd2b057926342fa07a1cd4daa34b12de77e482e64f7bfb7f2ba19eb3918804` |
| `HDPMI32I.EXE` | `93785e0f1ff2de30d952b57eab3ba1c8f34f9963f06185c42ca4ca6d4c71c8c5` |
| `JEMM386.EXE` | `ba16835eb69aee1646fc04beed4ba67136ed4bc1cd26b4678d1782b13420f9af` |
| `JLOAD.EXE` | `93c205814779baf2f9c89d45fe1e60a3b7e597a13a87cfb7538f7cc5b108af40` |
| `DPMIRUN.COM` | `f9d609891674180a6e3e6da3d155eb1fa48d49837873e3a7282d9e3c4fbf88e7` |
| `SHELL.COM` | `79e879a8f2348394c1708146c3064cb7253924cc4faef7b4d8e6d2cd1e2d57c7` |
| `SHELL-rebuilt.COM` | `79e879a8f2348394c1708146c3064cb7253924cc4faef7b4d8e6d2cd1e2d57c7` |
| `CVSESSION.DLL` | `99122776d4e38dde46cb6387d893cecade3d75b93fc288754e689f3a10fafb38` |
| `DOSWIN.DRV` | `e8f440feb9498c720f01f19bbc36c3b962d26db6f7472fdc31de3c0760d3c106` |
| `ciukidos.sys` | `2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af` |

## Results: all 20 gates pass

| Gate | Result |
| --- | --- |
| `unit-peripherals` | PASS |
| `unit-virtual-vga` | PASS |
| `unit-vga-x86` | PASS |
| `unit-scheduler` | PASS |
| `unit-device-link` | PASS |
| `unit-lifecycle` | PASS |
| `unit-jemm-query` | PASS |
| `vga-session` | PASS |
| `legacy-v86` | PASS |
| `legacy-dpmi` | PASS |
| `v86cli` | PASS |
| `devtest` | PASS |
| `lifetime` | PASS |
| `vga-window` | PASS |
| `dos-window-text` | PASS |
| `dpmi-probe` | PASS |
| `doom-vanille` | PASS |
| `doom-original` | PASS |
| `doom-vanille-sfx` | PASS |
| `doom-original-sfx` | PASS |
