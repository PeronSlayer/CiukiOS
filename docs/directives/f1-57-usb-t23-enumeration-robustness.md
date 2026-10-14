# f1-57 — USB dead on the T23 (image fb3147dc): stick never enumerates, live receiver quarantined after a port re-enable

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-57` from `main`. Host-testable; the lead runs `f1-usb` on QEMU and the T23 test.

## Evidence (T23, three ICH3 UHCI controllers 00:1d.0/1/2, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/fb3147dc/disk-log/BOOT.LOG.clean`)

1. **Lexar stick 05dc:a81d, present at boot, controller usb-2493 (00:1d.1) port 2.** `port=2 status=0000009b event=connect` at tick 7757 (CCS, CSC, PEDC, D+ line; PE=0). After the 100 ms debounce and `port_reset` the first control transfer failed within 4 ms: `addr0 uhci_packet pid=2d command=00c1 status=0002 frame=1935 td=00450007` = SETUP retired with CRC/Timeout + Stalled, error counter exhausted, 8 bytes sent, no handshake; `addr0 get_descriptor ... result=-5 elapsed_ms=4`; `quarantine device addr=0 reason=enumeration_transfer stage=enumeration`; later `port=2 status=00000091 event=connect reason=quarantined` (PE=0). `usb_enumerate` makes exactly one attempt and `usb_core.c:400-401` retains the failure "never retry". The same stick failed on every previous T23 image (e86305de, 293c05af, 8c88b1ab); it is a USB 2.0 device running full speed behind a UHCI root port, and the BIOS (legsup_before=0010) had enumerated it before the handoff.
2. **Logitech receiver 046d:c52b plugged at tick 22475 on port 1 of the same controller.** Enumeration fine (`get_descriptor result=8`, `device addr=1`, HID keyboard interface 0 ep 1 interval 8 and mouse interface 1 ep 2 interval 2 attached, `[input] devices=3`). The HID polls then returned `result=-11` with `td=188807ff` (TD still Active, NAK, C_ERR=3): an idle HID device NAKs its interrupt IN, this is normal. At tick 23499 the root scan logged `port=1 status=00000095 event=enable reason=state` (PE changed on a port with a live child, about 850 ms after enumeration) and the next poll on interface 1 ep 2 retired `td=18500002` (Babble + Stalled, 3 bytes) → `result=-5` → `quarantine device addr=1 reason=hid_transfer stage=enumeration`, `[input] ... event=detach` twice. In image 293c05af (before f1-52/f1-56) this receiver worked on the T23.
3. **QEMU `f1-usb` on fb3147dc:** `usb-input-uhci` PASS, `usb-input-ohci` FAIL "USB input consumption stalled action=0 batch=2 repeat=0 step=0" (`build/lead-scratch/main-f1-usb-rerun.log`).

## References to check first

USB 2.0 §7.1.7.3 (connect debounce 100 ms), §7.1.7.5 (reset ≥10 ms from a root hub, TRSTRCY 10 ms recovery before the device must answer), §9.2.6.3 (TDSETADDR 2 ms); UHCI Design Guide 1.1 §2.1.7 PORTSC (W1C CSC/PEDC, PR, PE) and §3.2.2 TD status (bits 17-23, C_ERR); Linux `drivers/usb/core/hub.c` `hub_port_init` (GET_DESCRIPTOR_TRIES=2 series × 3 operations, a new port reset between series, `msleep(100)` between operations, 10 ms after SET_ADDRESS), `hub_port_debounce`, `hub_port_reset`; Linux `drivers/usb/host/uhci-hub.c` (PR cleared, 10 µs, PE set explicitly: Intel chips do not enable the port by themselves); USB HID 1.11 and USB 2.0 §9.4.5 (a STALL on an interrupt endpoint is cleared with CLEAR_FEATURE ENDPOINT_HALT, data toggle reset).

## Task

1. **Enumeration robustness as in Linux** (`src/kernel/usb/usb_core.c`, `uhci.c`, `ohci.c`): debounce, reset, 10 ms recovery, GET_DESCRIPTOR(8) up to three operations 100 ms apart, on failure a second reset and a second series, SET_ADDRESS with a 2 ms settle; quarantine `enumeration_transfer` only after that. A device the BIOS already addressed returns to address 0 only through the reset: model it in the host UHCI model (device that ignores address 0 until reset; device that answers the first SETUP after reset with a timeout, then works).
2. **Never reset or disable a port with a present, non-quarantined child** unless CCS dropped. Find what produced `event=enable reason=state` at tick 23499 and the babble that followed: candidates are the scan paths in `usb_core.c:430-500` (`root_port_changed` detach, `(s ^ previous) & USB_PORT_ENABLED`), `uhci.c port_write()` W1C handling `(v & ~0x0a)` versus `reset()`'s final `| 0x0a` write, PEDC raised by the controller (babble/overcurrent disables a port: re-enable without reset and record it). Reproduce in the host model, fix, test.
3. **HID interrupt IN:** NAK is never a failure; the TD stays in the periodic schedule until data; a STALL gets one CLEAR_FEATURE(ENDPOINT_HALT) + toggle reset + retry before quarantine; babble likewise once. Keep the -11 trace limit.
4. **OHCI runner failure:** reproduce the stall at batch 2 on the host OHCI model (periodic ED/TD re-arm after the first batch?). Fix the driver if it is the driver; if it is runner pacing, say so with the evidence.
5. **Records:** every root port reset/enable/disable with the raw PORTSC value and every enumeration attempt number at `klog` level (the T23 disk log is the only evidence we get). Rate-limit as f1-53 did.

## Acceptance

`tests/host/test_runner_usb.py`, the USB host models and the kernel host suite green; the lead runs `f1-usb` (UHCI and OHCI cases) on QEMU and the next T23 test must show the stick enumerated (`usb_storage` bound, Files shows it) and the receiver's keyboard and mouse alive for the whole session.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time (the workstation is short of memory). Research the references on the internet first (`--search` is enabled), record the links and the decision in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-57-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify on QEMU and on the T23.
