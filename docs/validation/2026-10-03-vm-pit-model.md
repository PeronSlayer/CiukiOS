# Per-VM PIT expiry accounting

## Sources

- Intel, *500 Series Chipset Family Platform Controller Hub Datasheet*, Vol. 1, §11.2, pp. 55–56: [official PDF](https://cdrdv2-public.intel.com/635218/635218-008.pdf). It describes mode 0 as a terminal-count one-shot, modes 2 and 3 as periodic/reloading modes, and counter latching as a read snapshot that does not stop counting.
- The same Intel section specifies the 8254 read-back command, selected counter count/status latches, and status layout including OUT and Null Count. A pending count or status latch is preserved until its read; if both are pending, status is returned first and count follows.
- Linux KVM's PIT implementation: [`arch/x86/kvm/i8254.c`](https://github.com/torvalds/linux/blob/master/arch/x86/kvm/i8254.c). It increments a pending-expiration count for periodic timer events and schedules another injection after IRQ acknowledgement when expirations remain.
- QEMU's PIT implementation: [`hw/timer/i8254.c`](https://qemu.googlesource.com/qemu/+/refs/tags/v9.2.0-rc0/hw/timer/i8254.c). It derives channel transitions from virtual-clock time and rearms the periodic timer at the next transition. Its mode-3 count read is `reload - ((2 * elapsed) % reload)`; QEMU itself marks this approximation as potentially incorrect for odd reload values.

## Decision

Keep the PIT model's channel-0 counter and its 8259 request bit separate from the number of elapsed terminal counts. One `cvgp_advance` call may span multiple periods, so the model must retain every channel-0 expiry in a saturating pending counter while the modeled PIC still exposes its normal single IRR bit. The session bridge can then inject at most one virtual IRQ0 while an earlier IRQ0 is pending or in service, consume one expiry only after Jemm accepts that injection, and offer the next expiry after the previous IRQ has completed.

Mode 0 adds one expiry and stops at terminal count. Modes 2 and 3 add `1 + (clocks - remaining) / reload` expiries once the first terminal count is crossed, then preserve the exact residual phase. A later counter reload does not retract an expiry already generated. The counter starts at the existing BIOS-like mode-3, divisor-65536 state. Pending expiries reset with the PIT/model lifecycle.

For channels 0–2, support both the per-counter count latch command and the 8254 read-back command. Count and status latches are independent, a second latch cannot overwrite an unread value, and a latched status is returned before a latched count. A captured count remains stable until it is read; loading a new count does not overwrite the unread snapshot. The temporary low byte of a two-byte write has separate storage from that snapshot. Status reports OUT, Null Count, RW mode, normalized operating mode and BCD. Null Count is set by a new control word and cleared only when the complete programmed count has been written; unsupported BCD/modes remain explicit model errors. Mode-3 count reads follow QEMU's formula above, including its documented odd-count approximation, while IRQ expiry timing remains based on whole reload periods.

Elapsed time remains owned by the caller and is advanced independently for each VM. The caller must preserve elapsed clocks beyond any bounded processing chunk; this model change does not choose a host scheduler cadence or modify physical PIT ownership. If the saturating expiry counter reaches `UINT32_MAX`, it stays saturated rather than wrapping and losing the oldest debt.

## Repository integration boundary

The public model API exposes the pending count and a one-expiry consume operation, both validating the model generation. Consume returns `1` only when it removes an expiry, `0` when the queue is empty, and a model error for an invalid owner. The Jemm/VMM bridge remains the authority for guest PIC pending/in-service state and decides when to queue IRQ0. This document records the model contract only; host PIT setup, per-VM instance lifetime, I/O routing and scheduler integration are owned by the session/VMM layer.

The session clock keeps a separate PIT/PIC model for each of the four VMs and advances each lazily from its own monotonic TSC baseline, including while another VM runs. It carries sub-millisecond TSC cycles and fractional PIT clocks across polls, and chunks elapsed milliseconds only to keep the freestanding 32-bit arithmetic bounded; no elapsed-time cap is applied. The model begins with BIOS mode 3 and divisor 65536. Root physical PIT restoration uses the guest's divisor and mode with normalized low/high access, while physical IRQ0 remains the independent host scheduling source.

The integrated model suite passed 1,215 assertions with ASan/UBSan and a freestanding OpenWatcom compile/link. The wrapper and Linux QEMU checks are recorded in [integrated validation](2026-10-03-independent-vm-clock.md).
