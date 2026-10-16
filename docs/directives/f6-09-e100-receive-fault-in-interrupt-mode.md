# f6-09 — e100 quarantines itself on the first interrupt in IRQ mode

- **Phase:** F6 (network). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f6-09` from main `5e494aa3`+.
- **Authority:** `src/kernel/drivers/net_e100.c`, `src/kernel/drivers/net.c` (netif IRQ/poll path only), `tests/host/net/`, `docs/validation/f6-01-nic-drivers.md` (amend).

## Evidence (image 1ff0a1fe, QEMU `qemu-t23` profile, case `i82559er-qemu-t23`, run `build/test-runs/zz-d4/e4da24ca/serial.log`)

```
[net] driver=e100 mac=52:54:00:12:34:56 link=up irq=11 mode=irq state=running reason=admission
CIUKI_TEST … probe=net-arp event=READY driver=e100 … mode=intx
[net] driver=e100 mac=52:54:00:12:34:56 link=down irq=11 mode=poll state=quarantined reason=receive_fault
CIUKI_TEST … case=arp arp=fail mac=00:00:00:00:00:00 tx=1 rx=0 dropped=0 errors=1
CIUKI_TEST … case=budget max_irq_us=0 violations=0 interrupts=1 quarantine=1
```

IRQ 11 is shared with the AC'97 controller (chain). The first interrupt is handled (`interrupts=1`), then `netif_poll` gets a negative length from `e100_poll` and quarantines the device (`net.c`, `receive_fault`). `rtl8139-qemu-t23` on the same image reports `reason=firmware_active` (separate, pre-existing admission refusal: record it, do not change admission policy here).

## Required

1. Reproduce on the host e100 register/DMA model (`tests/host/net/`) in IRQ mode: ARP transmit, interrupt, `e100_interrupt` (mask + W1C), worker poll. Identify which return of `e100_poll` fires: the transmit deadline (`cb->status` complete but SCB CU state bits 7:6 not idle, or the CB never completing), the RFD count flag check (`F`/`EOF` bits 0x4000/0x8000, QEMU writes `0xc000 | size`), `status == 0xffff`, or `e100_command` timing out while the SCB command byte is still busy. Check whether the 8-bit interrupt-mask write to SCB byte 3 (`e100_irq_enable`) can race a pending CU/RU command in byte 2, and whether the ack write of byte 1 inside `e100_poll` is correct after `e100_interrupt` already acknowledged the same causes.
2. Fix the driver against the Intel 8255x developer manual (SCB command/status word layout, CU/RU state bits, RFD/TCB bit definitions) and QEMU's `hw/net/eepro100.c` as the reference model; polling mode must keep its current behaviour; a lost or spurious interrupt must not quarantine the device, only a real DMA/register fault may.
3. Host coverage: IRQ-mode ARP round trip on the e100 model with the chain's mask/W1C order, interrupt before and after CB completion, RFD completion with and without the F/EOF bits, and the polling path unchanged. Run `tests/host/net/run.sh`, then `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh` and `python3 scripts/build_kernel.py`.
4. Report in `build/codex/f6-09-report.md`: the exact failing return with the register values, the fix (file:line), the sources, verbatim results. The lead runs `i82559er-qemu-t23` and `f4-audio` on the integrated image.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
