# f1-102 — Universal peripheral drivers: census of input, serial, parallel, game port, floppy, USB, power and audio coverage, with a port plan

- **Phase/model:** F1/F4, Codex gpt-6.1-sol, reasoning high, `--search`. **Worktree:** `wt/f1-102` from `main`. Research and contract only: **no change under `src/`**.
- **Owner request (2026-10-22 ~23:20):** include the research and integration of universal drivers for peripherals such as mouse, keyboard, serial ports and the rest, so that CiukiOS works on any retro PC and not only on the T23.

## Today (lead census, 2026-10-22)

| Class | Today |
| --- | --- |
| PS/2 keyboard and mouse | `src/kernel/drivers/i8042.c`: raw set 2, three-byte mouse only. Wheel (IntelliMouse) and five-button (Explorer) negotiation are refused; touchpads are used only in PS/2 compatibility mode. |
| USB | UHCI and OHCI host controllers, hub, HID boot keyboard and report-parsed mouse, mass storage. No EHCI, no HID gamepads or joysticks, no keyboard consumer keys. |
| Serial | `src/kernel/core/serial.c`: 16550 transmitter for the kernel log only. No user COM API, no serial mouse, no COM passthrough to DOS VMs. |
| Parallel, game port, floppy controller | None. |
| Power | ThinkPad EC only (`thinkpad_ec.c`, SMBIOS-gated). No generic ACPI or APM power button, lid, battery or soft power-off. |
| Audio | AC'97 (`ac97.c`), VIA 82xx, ES137x, SB16. |
| Storage | Parallel ATA with legacy ports. |

## Task

1. **For each class, list what a 1998–2004 PC commonly has.** Use `docs/design/hardware-baseline.md`, the owner's driver pack hardware ids (`assets/hwids/*.json`, Media/HID/Ports/USB/System classes) and the period's chipset lists (Intel PIIX/ICH, VIA, SiS, ALi, nVidia nForce). Identify the gaps against today's drivers.
   - **Input:** the PS/2 wheel and Explorer protocols (Microsoft IntelliMouse specification, Linux `psmouse`), Synaptics and ALPS touchpads in their absolute modes (optional), USB HID report-protocol keyboards with consumer keys, and USB and game-port joysticks and gamepads.
   - **Serial:** 8250/16450/16550/16550A/16650/16750 detection; COM1–COM4 resources from the BIOS data area, ISA PnP and PCI serial cards; serial mice (Microsoft, Logitech, Mouse Systems protocols, and Microsoft's "Plug and Play External COM Device Specification"); a user COM device; and COM passthrough to DOS VMs.
   - **Parallel:** IEEE 1284 SPP, EPP and ECP; LPT ports for DOS VMs, such as printers and dongles.
   - **Game port:** port 201h analog joysticks, native and virtualised for DOS VMs.
   - **Floppy:** 82077AA-compatible controllers, read and write, for DOS VMs and media.
   - **USB:** EHCI with companion-controller handoff; OHCI/UHCI quirks by chipset.
   - **Power:** ACPI through ACPICA (Intel's reference implementation, dual BSD/GPL licence; verify the exact terms), covering power button, lid, battery, thermal and soft-off, compared with APM 1.2 for 1998–2000 machines. Treat the ThinkPad EC as a vendor quirk on top of the generic path.
   - **Audio gaps:** EMU10K1 (SB Live!/Audigy), CMI8738, Yamaha YMF7xx, ESS Maestro, Trident 4DWave, SiS 7012, nForce and ALi AC'97 variants, with the codec list.
   - **Storage gaps:** chipset ATA DMA modes, Promise and HighPoint add-in controllers, AHCI (out of the baseline: say so).
2. **Candidate sources per gap, with exact upstream paths and tag.** Consider Linux (GPL-2.0, compatible with the GPL-2.0-only kernel), FreeBSD and NetBSD (BSD), Haiku (MIT) and ACPICA. Read the licence header of every file a port would take and report every exception. Give the capability, size estimate, host API needed and test path for each: QEMU devices such as `usb-ehci`, `pci-serial`, `isa-parallel`, `isa-fdc`, wheel PS/2 mouse, `ich9-intel-hda` (out of scope) and the `-device` list; otherwise hardware only.
3. **Driver-model check.** Check the kernel's device model (`docs/design/device-firmware-ownership.md`, `docs/design/device-manager.md`, registry claims) against each new class: claims, quarantine, Device Manager records, and DOS VM virtualisation (`docs/design/unified-dos-contract.md`). Propose any extension.
4. **Contract** `docs/design/peripheral-driver-coverage.md`: the gap table, the chosen source per gap with its reason, the order of directives f1-103… (most common first), the user-visible result of each, and an honest expectation of what QEMU can verify versus what needs real hardware.

## Acceptance

The contract document is complete, every source carries a link and a licence verdict, and nothing under `src/` changes.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Downloaded sources go under the worktree's `build/research/` (ignored) and are never committed. Write `build/codex/f1-102-report.md`.
