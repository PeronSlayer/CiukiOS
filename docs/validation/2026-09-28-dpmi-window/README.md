# DOS/4GW games in the DOS window — 28 September 2026

Reports from `scripts/test_vm_window_profile.sh --image build/full/ciukios-full.img`
(output `build/tests/vm-window-profile-2026-09-28b`), on an image built with
`CIUKIOS_VM_WINDOW=1 scripts/build_full.sh`. QEMU with KVM, `-cpu pentium3`,
AC'97. Nothing here qualifies physical hardware. Design and limits:
[record](../../vm-dpmi-window-devices-2026-09-28.md).

## Artifacts under test (taken from the image)

| File | SHA-256 |
| --- | --- |
| `ciukios-full.img` | `2e8503f5e6ab4e75fcf9b54d99a798eccfa00024be909034eb17ccf10738f432` |
| `HDPMI32I.EXE` | `3dbc9df03ca8508ed931b50f3422dce53c79c8d03929f662827113c843f0c672` |
| `JEMM386.EXE` | `68ed772e5f5f489f9854c98fecdde3c54a6e3a12ce4fee51287850fe6f14eb73` |
| `JLOAD.EXE` | `93c205814779baf2f9c89d45fe1e60a3b7e597a13a87cfb7538f7cc5b108af40` |
| `DPMIRUN.COM` | `7796325b436fc5df74c6309758ffa0d3bca958d7b9777ba1adbdc7db26230046` |
| `SHELL.COM` | `e3d6882b01d86a4d74fb2a32aa0cdc9b3dac3ad8a96bd9b41a6ce5a5d13944c7` |
| `SHELL-rebuilt.COM` | `e3d6882b01d86a4d74fb2a32aa0cdc9b3dac3ad8a96bd9b41a6ce5a5d13944c7` |
| `CVSESSION.DLL` | `fc586c968aa41ebdf677b2e192114e8eefc83ad70008e5549d241a7e3030665d` |
| `DOSWIN.DRV` | `e8f440feb9498c720f01f19bbc36c3b962d26db6f7472fdc31de3c0760d3c106` |
| `ciukidos.sys` | `2f53b6e63ccaa04f496b54c29f7e74d3628b1316245e180f21149269173380af` |

## Results: all 20 gates pass

| Gate | Result | Detail |
| --- | --- | --- |
| `unit-peripherals` | PASS | |
| `unit-virtual-vga` | PASS | |
| `unit-vga-x86` | PASS | |
| `unit-scheduler` | PASS | |
| `unit-device-link` | PASS | |
| `unit-lifecycle` | PASS | |
| `unit-jemm-query` | PASS | |
| `vga-session` | PASS | |
| `legacy-v86` | PASS | |
| `legacy-dpmi` | PASS | |
| `v86cli` | PASS | |
| `devtest` | PASS | |
| `lifetime` | PASS | |
| `vga-window` | PASS | |
| `dos-window-text` | PASS | |
| `dpmi-probe` | PASS | |
| `doom-vanille` | PASS | 1042/1236 SB IRQs delivered, loudest RMS 5770 |
| `doom-vanille-sfx` | PASS | 829/1156 SB IRQs delivered, loudest RMS 5792 |
| `doom-original` | PASS | 1096/1196 SB IRQs delivered, loudest RMS 2417 |
| `doom-original-sfx` | PASS | 1131/1233 SB IRQs delivered, loudest RMS 2242 |

The `doom-*` gates check game drawing, keyboard, SB blocks, OPL (music runs),
protected-mode IRQ delivery, model errors/underruns, audible AC'97 output,
F10+Y exit with host unload, and the released IRQ claim. `doom-original*`
use the image's own `\VM\VMSTART.COM` and `\VM\DPMIRUN.COM`.

## Failures preserved

- `failure-dos4gw-gp-v86-injection.json`: device IRQs injected into V86 under
  DOS/4GW's handler: `DOS/4GW error (2001): exception 0Dh ... at 97:0000751A`.
- `failure-hdpmi-c10f-lss.json`: the original DOOM stopped at `I_StartupTimer`
  with `hdpmi: fatal exit C10F` (the virtual-CLI tracer refused `LSS`).
- `failure-devtest-sb-dc-hold.json`, `failure-vga-window-sb-dc-hold.json`
  (from `build/tests/vm-window-profile-2026-09-28`): the first speaker-gate
  change kept mixing the last SB sample after DMA stopped; the FM note was
  not detected. Fixed; DSP stop/pause now outputs silence.
