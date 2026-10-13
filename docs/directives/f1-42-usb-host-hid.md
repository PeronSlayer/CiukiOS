# f1-42 — USB: UHCI and OHCI host controllers, HID keyboard and mouse

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-42` from `main`. Host-testable with synthetic controllers; the lead runs QEMU (`-device piix3-usb-uhci`/`pci-ohci` with `usb-kbd`, `usb-mouse`) and hardware. Owner directive 2026-10-13: a native driver park for the common hardware of 1998–2004; USB keyboards and mice are the first gap (2000+ desktops, laptops without working PS/2 emulation).

## Phase 1 — contract `docs/design/usb-contract.md` (before code)

Scope: UHCI (Intel PIIX3/PIIX4/ICH, VIA) and OHCI (Compaq, ALi, nVidia, SiS) host controllers found by PCI class 0C/03; root hubs, external hubs one level; control and interrupt transfers; HID boot protocol for keyboards and mice (report protocol later); device enumeration records; legacy-emulation hand-off (UHCI legacy support register, SMI disable) and the firmware-first rule of the i8042 contract (never break a keyboard the firmware already provides; the owner's Armada E500 depends on it); isolation and quarantine like the other drivers (bounded transfers, no DMA before descriptors, registry claims of ports/memory/IRQ, work-queue budget); the input path joins the existing keyboard/mouse queue of the kernel so the desktop sees one input device model; EHCI companion routing (route to the companion controllers; no EHCI driver yet). Sources: UHCI design guide 1.1, OHCI spec 1.0a, USB 1.1/2.0, HID 1.11 and the HID boot protocol usage tables; cite them.

## Phase 2 — implementation

- `src/kernel/usb/`: `usb_core.c` (device model, enumeration, descriptors, pipes), `uhci.c`, `ohci.c`, `usb_hub.c`, `usb_hid.c` (keyboard with LED and typematic through the kernel's existing repeat policy, mouse with wheel).
- Verbose records: `[usb] controller=… ports=…`, `[usb] device addr=… vid:pid class=… speed=…`, `[usb] hid keyboard/mouse attached`, quarantine records with reasons.
- Device Manager shows the controllers and devices (ids from the hwids database).
- Host tests: synthetic UHCI and OHCI register/DMA models in `tests/host/usb/` driving enumeration, hub ports, HID reports, error and timeout paths, DMA bounds.

## Acceptance

Host suite green; i686 kernel compiles; the lead boots `qemu-desktop-2002` with USB input only (no i8042) to the desktop and types.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-42-report.md`.
