# f1-71 — Shared PCI interrupt lines: USB and network run in polling mode because the audio holds IRQ 11 (T23, image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-71` from `main`. Build the kernel and the DOS image before reporting.

## Evidence (`disk-BOOT2.LOG` lines 61–69, `disk-BOOT.LOG` lines 424–441)

- `[registry] conflict: usb-3217 wants 1:b-c held by ac97 reason=overlap` for all three UHCI controllers; `[usb] controller=uhci … mode=poll reason=ready_polling` and `[net] driver=e100 … irq=11 mode=poll`: on the T23 the AC'97 controller (8086:2485), the three UHCI controllers (00:1d.0–2) and the Intel PRO/100 share PCI line 11, and the registry's exclusive lease gives it to the first claimant; every other device falls back to polling (USB scans 4 times per second: mouse lag, slow stick, the H page's USB items waiting for a first scan).
- On the H page boot the audio codec reset failed (`codec=43525936 … result=failed access=cas_timeout`, `error=-110 reason=stop_unproved`) and the audio still held the lease.

## Task

1. Shared level-triggered PCI interrupts: the registry leases a PCI line to several PCI functions (INTx is level-triggered and shareable by design; ISA edge lines stay exclusive); the dispatcher chains the handlers of a line and each driver acknowledges only its own device's status (UHCI USBSTS, AC'97 status, e100 SCB); spurious/unhandled counters per line; EOI once per interrupt after all handlers ran. Reference: PCI Local Bus Specification interrupt sharing, the Linux `request_irq(IRQF_SHARED)` model, the drivers' own status registers (Intel ICH3 datasheet, UHCI 1.1 specification, 82557/8/9 manual), and the vendor INFs in the pack (`e100b325.inf`, `Alcwdm.inf`) for the IRQ expectations.
2. USB controllers and the NIC in interrupt mode on shared lines, keeping polling as the fallback when a line cannot be shared; the USB worker's cadence then follows the controller's interrupts (1 ms frames) for HID and bulk transfers.
3. Audio: a failed codec reset must release the line and the controller (no lease held by a failed device), with the reason recorded.
4. Host fixtures: three devices on one line with interleaved interrupts, a device asserting while another's handler runs, a failed device releasing its lease.

## Acceptance

Kernel host suite green; QEMU `f1-usb`, `f4-audio`, `f6` network cases green (QEMU's PCI devices also share lines); next T23 log: no `[registry] conflict`, USB and e100 `mode=irq`, mouse smooth, stick fast.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`; cite the sources in `build/codex/f1-71-report.md`.
