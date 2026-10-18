# f0-39 — one member's self-fault quarantines the whole shared IRQ 11 line: USB, mouse and AC97 die after an SMI stall in the e100 handler

- **Phase:** F0/F1. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-39` from main (after `ffeca72b`).
- **Authority:** `src/kernel/core/registry.c` (IRQ chains), `src/kernel/drivers/net.c` (`netif_irq` budget), `src/kernel/usb/usb_core.c` (`lease_invalid`, IRQ loss), `src/kernel/core/hardware_check*.c` (SMI measurement of f0-35), `docs/design/device-firmware-ownership.md`, `usb-contract.md`, host tests `tests/host/` (registry/IRQ chain, net, usb).

## Evidence (T23, image 041b221f, `legacy/local/physical/041b221f/BOOT.LOG` lines 391–401, Normal boot, read-only)

```
[net] driver=e100 mac=00:d0:59:b6:43:8f link=down irq=11 mode=irq state=quarantined reason=irq_or_registry_fault
[irq] budget exceeded irq=11 owner=net-1690 us=2123 pending=3
[irq] quarantined irq=11 owner=ac97 unclaimed=0
[irq] quarantined irq=11 owner=net-1690 unclaimed=0
[irq] quarantined irq=11 owner=usb-1694 unclaimed=0   (and usb-1698, usb-1702)
[usb] quarantine controller=usb-1694 reason=lease_invalid irq_isolated=1   (×3 controllers)
[input] devices=2 source=usb event=detach ps2=2 usb=0 firmware=0
[storage] readonly reason=device_quarantined file=Hallelujah_cover_girls.mp3 cluster=0
[audio] idle-tail transport stop err=-5
```

- Owner: the music player opens the MP3 from the stick and nothing plays; shortly after, the USB ports and the USB mouse stop working. Both are this cascade: the stick's controller and the AC97 were quarantined with the line.
- `net.c:135`: `if (us > 50) { budget_violations++; fault_pending = true; }` → `quarantine_locked(d,"irq_or_registry_fault")` → `registry_quarantine()` of the e100 leases. `registry.c:457-459`: a chain member whose lease is not ACTIVE/QUIESCING quarantines the **whole line** (`chain_quarantine(irq)`), so every IRQ 11 owner (three UHCI, AC97, e100) lost its lease; `usb_step` then quarantines each controller (`lease_invalid`).
- The 2123 µs overrun is the T23's periodic SMI stall (f0-35: ~2.1 ms every 1–3 s, `irqoff_max_us=2130` in every boot), which lands in whichever handler is running; it is not the e100's work.

## Required

1. **Shared-line policy.** A member whose lease is released or quarantined is detached from the chain (handler skipped, member cleared, `[irq] member_detached irq=%u owner=%s`); the line is quarantined only when it is stuck (`IRQ_CHAIN_STUCK_PASSES` unclaimed passes) or when the line's own registry state is inconsistent. The other members keep running. Document the rule in `device-firmware-ownership.md`.
2. **Budget attribution.** A single overrun whose magnitude matches the platform's measured SMI stall (use f0-35's measurement/tracer: stall ≈ 2.0–2.3 ms with no handler work in between) is recorded as `smi_attributed` and is not the handler's fault; the e100's 50 µs self-check and the chain's `budget exceeded` follow the same rule; only repeated unattributed overruns (N in a window, document N) quarantine a member. The 250 µs aggregate chain deferral to thread context stays.
3. **USB resilience.** When a UHCI/OHCI controller loses its IRQ lease it falls back to frame polling (`[usb] irq_lost controller=%s mode=poll`): HID keeps delivering events, storage keeps working at the worker cadence; no `lease_invalid` quarantine for a line event.
4. **Host coverage.** Chain tests: one member quarantined → line alive, others served; stuck line still quarantined. `netif_irq` with an injected 2.1 ms stall (SMI signature) vs a genuine long handler. USB controller IRQ loss → polling fallback with HID events still delivered (`tests/host/usb`). `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
5. Report `build/codex/f0-39-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No runtime PIC writes beyond the existing mask/unmask rules (f0-33). No new register programming.
