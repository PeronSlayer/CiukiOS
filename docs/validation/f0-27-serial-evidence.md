# F0-27 serial evidence admission

Research checked on 2026-10-15 before implementation:

- [QEMU x86 CPUID implementation](https://github.com/qemu/qemu/blob/master/target/i386/cpu.c)
  returns `TCGTCGTCGTCG` in EBX/ECX/EDX for leaf `0x40000000` under
  exposed TCG. Use the same signature and hypervisor-bit guard as
  `src/kernel/core/mtrr.c`, independently of the selected probe.
- [QEMU UART implementation](https://github.com/qemu/qemu/blob/master/hw/char/serial.c)
  sends transmit bytes through `qemu_chr_fe_write` in `serial_xmit`.
  Its divisor configures backend parameters; normal transmit forwarding
  does not wait for a physical 8N1 character period. Keep THRE and verified
  FIFO gating, but remove physical-line admission and per-tick byte pacing
  for TCG or a loader-validated fw_cfg test request.
- [QEMU fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html)
  defines user-supplied `opt/` files. Ciuki's validated boot handoff already
  represents the test selector with `CBI_F_TEST_REQUEST`; reuse that flag,
  without adding firmware I/O or requiring a selector on normal TCG boots.

The read-only failing run `f0-smoke/1c5c9798` used image
`c074bb8f44a1d25df903f0fe219eb6bc264367fa3c24a73ad83b98b88602b115`,
revision `d49004033f5d01a91729c17b4e68a4314dcec3e2`. Its 99 serial lines
include a kernel banner and `serial_drop=16`, but neither supervisor desktop
milestone required by `scripts/test/run.py`. The runner reads 65536-byte
chunks from its UART FIFO; it imposes no physical baud limit. F0-24 applied
2816 expanded data bytes per 1000 ticks indiscriminately to ordinary boot
logs. `klog` and nonperiodic `rec_diag` lacked evidence admission.

Decision: boot/event `klog` and nonperiodic probe records use protected
admission, as `rec_emit` already does. Preemptible tasks sleep for quota
and queue capacity before publication, with evidence sequence assignment
after any wait. Before task startup or in atomic context, protected
publications bypass quota rather than waiting with interrupts disabled;
they still use the bounded byte/descriptor queue. Oversize or exhausted
atomic queues retain the existing whole-record failure accounting; no
finite queue can promise delivery with a stopped consumer. Periodic
diagnostics retain whole-record quota drops. Physical hardware keeps the
75% quota and wire pacing; TCG/test boots bypass both rate limits while
retaining one writer, FIFO gating and one byte per IRQ lock. The writer
still yields after each verified FIFO refill (at most one refill per tick);
the chardev exemption removes the physical three-byte cap, not this bounded
scheduling cadence.

Host verification and the QEMU handoff are recorded in
`build/codex/f0-27-report.md`. QEMU acceptance remains with the lead.
