# fx-dospic: virtual IRQ completion and early EOI

## Primary-source comparison

The Windows 95 DDK's [VPICD_IRQ_Descriptor](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S34DA.HTM)
separates virtual interrupt, EOI and IRET callbacks. Its
`VID_IRET_Time_Out` initializer is 500 ms; the timeout prevents an
unreturned simulated interrupt from hanging the VM. The lead's
[VID_IRET_Proc reference](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S34D4.HTM)
could not be fetched during this work. Microsoft's earlier
[DDK Virtual Device Adaptation Guide, chapter 37](https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/)
documents the same descriptor and callback protocol: VCD waits for IRET
after delivering a character, even if the guest issues EOI and STI early;
the virtual timer uses the same approach. The timeout callback is distinct
from the virtual EOI callback. These are Windows enhanced-mode/9x DOS VM
semantics, not Windows NT NTVDM semantics.

In this tree, `dosvm_pic.c` correctly clears ISR on guest EOI. Previously,
`dosvm_pit.c` allowed each new edge immediately and held an owed edge only
until EOI plus half a PIT period. Neither condition proves handler return.
DMX's early EOI therefore allowed recursive timer delivery when the host
stretched one timer handler beyond its approximately 7.1 ms period.

## Implementation decision

Hold each tracked IRQ's new requests until its handler returns or its
record expires. Keep PIC register/priority/EOI semantics intact. Count PIT
edges in the existing four-edge owed queue, including new edges while a
handler runs, and release them at the existing half-period pace. Other
devices retain the PIC's single-request coalescing semantics in a deferred
request bitmap. Different IRQs may still nest. Existing keyboard drains,
NE2000 cancellation, PIC initialization and PIT reprogramming also cancel
their corresponding held requests, so they cannot reappear after return.

DPMI uses the existing `DPMI_RF_HWINT` records, including protected-mode
handlers, real-mode reflection and exact-context return resynchronization.
V86 delivery records the reflected IRQ's SS:SP; only a successfully
emulated IRET from that frame closes it. A nested software IRET does not.

The lead's review notes explicitly select the existing `DPMI_ISR_WAIT_MS`
bound (50 ms, F3-44), so both paths retain that bound. This follows the
VPICD configurable-timeout model but differs from its 500 ms initializer.
Expiry releases only the delivery guard: it does not forge EOI, restore IF
or destroy a DPMI continuation. A deliberately reentrant same-IRQ handler
now receives the next request after IRET (or timeout); this is the intended
Win9x-style behavioral change. Ordinary handlers that finish before the
next edge follow the same delivery path and timing as before.

## Next T23 comparison

Compare the same game phase and duration, windowed and full screen, using
the new image's SHA-256. Preserve serial and disk logs. Record:

- `[vm-audio] cadence_clock`: lifetime `irq0_reinjected`, `irq0_lost`,
  `irq0_owed_max`, and the clock `gap_max_ms`/`dsp_lag_max_ms`.
- `[kernel] stretch_max_ms` and its task, plus the timer ISR duration and
  expiry counters in the VM performance records.
- DOOM music tempo and game tick progress before/after an observed stretch
  beyond one timer period. A bounded backlog should drain; `irq0_lost`
  should stay zero for the 5/8/11/20 ms replay envelopes. A host replay
  does not establish physical audio quality or tempo.

Host commands, measured replay counters and remaining lead validation are
recorded in `build/codex/fx-dospic-report.md`.

## Host observations

The six-mode DMX protocol replay injects one early-EOI host stall per 100 ms
for five seconds, followed by a 32 ms drain interval. Each 5/8/11/20 ms
case delivers all 704 PIT edges, loses zero and never nests the same IRQ.
The maximum owed queue is respectively 0/1/1/2. Separate checks cover
49/50 ms expiry with and without EOI, late continuation returns, nested
different IRQs/software IRETs, half-period pacing, the four-edge overflow
bound, cancellation and native exact-context resynchronization.

The full DOS harness reaches `PASS (61873592 checks, max record 237 bytes)`.
The existing 2,501-case DOOM audio replay retains `merged_cases=2375`,
`source_underruns=0`, `max_transport_underrun_frames=64` and its exact
waveform oracle. Its simulated CPU now executes IRET after EOI. Tests that
used to inspect an in-handler same-IRQ IRR bit inspect the deferred request
instead; tests of a long CLI/UART interval now return from the additional
already-paced tick released at IRET.

The 15 kernel latency tests, DOS file API suite, nine DOS runner tests and
compilation of the six changed production units with the canonical i686
flags pass. This sandbox prevents LeakSanitizer's thread inspection: the
full DOS process reports its functional PASS, then exits 1 in LSan, as does
a trivial sanitized empty `main`. Leak detection remains enabled wherever
the repository enables it. H56/H57 stop before CPU boot because the pinned
32-bit Open Watcom `wmake` is rejected with `Bad system call`. These are
incomplete qualification results; the lead must rerun the affected suites
in its supported host environment. No QEMU, image build or hardware result
is claimed.
