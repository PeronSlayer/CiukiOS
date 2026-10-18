# F3-42: guest ISR return latency

Research before implementation, 2026-10-18. The directive names behavior,
not source URLs. Primary references consulted:

- [Intel SDM, Vol. 2A, IRET and HLT](https://cdrdv2.intel.com/v1/dl/getContent/671199):
  same-ring IRET changes IF only when CPL <= IOPL; HLT at CPL 3 faults.
- [DPMI 0.9, sections 2.3, 2.4.1 and 3.1.2](https://www.phatcode.net/res/262/files/dpmi09.html):
  hardware handlers enter with virtual interrupts disabled on a locked
  stack; nested handlers share that stack. IRET/POPF need not restore IF.
- [DPMI virtual interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.1.html)
  and [hardware interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.2.html):
  physical IF can stay enabled throughout a client's virtual CLI region;
  it cannot be copied back into virtual IF at arbitrary host exits.
- [Microsoft's Win95 implementation account](https://devblogs.microsoft.com/oldnewthing/20160411-00/?p=93281/):
  saved-image TF observes a legacy POPF restore one instruction later.
  This concerns Windows 95 DOS sessions, not Windows NT NTVDM.

Repository comparison: `dpmi_enter_pm_handler` already constructs an IRET
frame targeting `DPMI_PM_IRQRET`, a host-owned HLT. `pmint_return` restores
the interrupted context and its virtual IF. Default reflection instead
returns via `DPMI_RM_RETURN` and `dpmi_v86_return`. V86 IRET/POPF are emulated
and apply their saved IF directly. PM CLI compatibility also arms selected
saved flags images with TF, and pending-IRQ recovery can step to a restore.

The baseline sound-on shooting replay meets the latency target but spends
19,843 debug traps outside the saved-image/return trackers, versus 848 with
`-nosound`. Pending-IRQ recovery is allowed inside active hardware frames,
including the IRQ0 mixer. Those frames intentionally disable virtual IF
and already guarantee observation through a host return trap. Exclude them
and their nested real calls/callbacks from recovery stepping and its timeout;
leave ordinary-code recovery separate. Preserve guest STI and verified
saved-image restores inside handlers, PIC masks, priority and EOI semantics.
The legacy timeout outside hardware frames remains a known compatibility
policy; no claim is made that it mirrors every unobserved ordinary POPF.

The four observed ordinary-code forced restores follow a save/CLI at
`01df:00247543`, outside any hardware frame. Their prior boundary seeds are
in distant game code; do not invent a backward instruction proof. The
baseline already passed the numeric host targets. Acceptance must therefore
include the reduction in recovery traps and exact nested-return tests, not
claim reproduction of the T23 40 percent window.

Decision: measure the existing paths first. Collect inclusive hardware ISR
duration (delivery to observed host return), nested depth, virtual-IF-clear
timer samples inside/outside hardware ISR records, and enable-path counts.
The host reports calibrated clock duration (thread CPU on the host) and
instruction-clock milliseconds; neither proves T23 wall latency. Sound-on
replay must demonstrate SB16 DMA, IRQ and block progress.

Implementation findings and before/after evidence are recorded in
`build/codex/f3-42-report.md`; QEMU and hardware evidence belong to the lead.

## F3-44 research and implementation decision (2026-10-19)

Rechecked the primary sources above before implementation, including Intel's
[80386 POPF reference](https://pdos.csail.mit.edu/6.828/2005/readings/i386/POPF.htm)
and [IRET reference](https://pdos.csail.mit.edu/6.828/2005/readings/i386/IRET.htm).
CPL 3 POPF/IRET silently preserve physical IF and IOPL; CLI/STI fault.
The libx86emu wrapper sanitized flags only at slice entry/exit, leaving
intermediate PUSHF instructions exposed to libx86emu's unrestricted restores.
Preserve IF/IOPL after each emulated instruction, and test inside the slice.

The corrected CPU baseline still delivers 700 IRQ0 and 430 IRQ7 in 5000 ms,
with 556 us average SB latency, 4 ms pending IF-clear and no merged blocks.
It does **not** reproduce the T23 failure. Do not manufacture a hardware
schedule or claim that this model correction reproduced the physical defect.

Keep verified saved-image TF and exact POPF/IRET retirement as the preferred
IF evidence. Replace the 100 ms ordinary-code fallback with a pending-IRQ
bound of two 1 kHz ticks (more than 1 ms), restarted by every CLI or DPMI
disable. Physical IF is never evidence for virtual IF. Reconcile hardware
records on monitor entry and before delivery: a consumed constructed frame
or an exact interrupted PC/stack retires the record without replacing the
current CPU frame. At 50 ms expire its hardware-ISR obligation, retaining
only a continuation for a possible late trampoline; this prevents both
permanent recovery suppression and a stale-return fault. Neither expiry
nor return evidence sends PIC EOI. Stack switches alone are not return proof.
The host trampoline remains the immediate, complete-context return path.
See the F3-44 rule in `execution-abi.md` and `build/codex/f3-44-report.md`.
