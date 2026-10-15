# f3-29 — DOS full-screen entry on QEMU rolls back on the VGA readback check of f3-26 (proposed)

- **Model/effort:** `gpt-6-luna`, `medium`. Not launched.

## Evidence

`vm-fullscreen-qemu-t23` (run `47c68fc2`, image `072d27f0`): `[display] fullscreen=1 mode=19`, then `[dosvm] fullscreen_transport_fault vm=1 registers=1`, `[dosvm] fullscreen_refused … error=-5`: the readback of the loaded VGA registers (f3-26, `vga_hardware.c`) disagrees on one register under QEMU's standard VGA, which does not store every register written, and the transaction rolls back; on image `94542c30` the same entry succeeded. The probe then ends before the runner's Alt+Enter stimulus.

## Task

Record which register and which bits disagreed; decide per register whether the mismatch is a QEMU deviation (document it in `docs/validation/2026-10-09-f0/README.md` like the other TCG deviations and mask it when the CPUID hypervisor signature is TCG) or a real transport defect; keep the check strict on hardware. Then apply f3-22's probe wait (300 s) so the case reaches the full-screen demo again.

## Acceptance

`vm-fullscreen-qemu-t23` and `vm-fullscreen-cirrus-qemu-t23` pass on QEMU.
