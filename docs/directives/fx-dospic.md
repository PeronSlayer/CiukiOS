# fx-dospic: timer interrupts held until the guest handler returns (Windows 95 VPICD model)

- **Written:** 2026-10-27 01:49 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Base: main 41732d190.
- **Evidence:** the Opus review of fx-doswin3 round 2 (/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/fx-doswin3-review2.md, final section) and the round-2 replay. DOOM's DMX sound driver sends its EOI early in its timer handler. A host CPU stall of more than about 7 ms while that handler runs can block the timer interrupt for good: tempo and game speed collapse. Today a new PIT edge is raised at once, and an owed edge waits only for the EOI (dosvm_pit.c:194-205), which DMX makes useless. The T23 trial shows an 11 ms stretch, so the hazard is real.

## Scope
1. Research and cite the Windows 95 VPICD virtual-interrupt model: VID_IRET_Proc runs on the VM's IRET or on a timeout, VID_IRET_Time_Out defaults to 500 ms (Windows 95 DDK, VPICD_Virtualize_IRQ descriptor and callbacks). Compare with this tree's virtual PIC and PIT delivery (src/kernel/vm/dosvm_pit.c and the virtual PIC code).
2. Implement the same rule for timer (IRQ 0) delivery to a DOS VM, and for any IRQ whose handler is tracked:
   - while the guest's handler for that IRQ is still running (from delivery until its IRET), new edges become owed instead of re-raising;
   - owed edges are released when the handler returns, with the existing half-period pacing;
   - the handler record times out like VID_IRET_Time_Out and falls back to today's rule.
   Cover V86 handlers and DPMI protected-mode handlers, both reflected and native, without changing the semantics of games that do not re-enter.
3. Tests:
   - a host replay of DMX's early-EOI pattern under injected host stalls of 5, 8, 11 and 20 ms: the tick count recovers, no edge is lost beyond what the pacing allows, no permanent stall;
   - the existing DOOM audio replay counters, explained if they change;
   - the existing DOS, DPMI, PIT/PIC, audio, unified DOS and kernel latency suites stay green;
   - run them one at a time with nice -n 19.
4. Write the records the lead should compare on the next T23 trial: owed/lost timer counts, stretch_max_ms, DOOM music tempo.

Report to build/codex/fx-dospic-report.md. Leave changes uncommitted.
