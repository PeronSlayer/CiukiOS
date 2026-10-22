# F6 foreign callback recovery (f6-05)

This is a partial implementation, not approval to execute an owner-pack driver.
The production entry gate remains closed while the prerequisites below are open.

## Research and implementation decisions

Intel's [SDM landing page](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
links [Volume 2A](https://cdrdv2.intel.com/v1/dl/getContent/671199), chapter 2
(legacy prefixes, opcode, ModR/M, SIB, displacement and immediate lengths),
and the CLI, HLT, INT and LGDT/LIDT entries. The
[Volume 2B](https://cdrdv2.intel.com/v1/dl/getContent/671241) MOV and STI entries
describe control-register writes and interrupt enabling. The scanner uses a
bounded legacy 32-bit length decoder; unknown encodings fail closed. It scans
the final relocated executable sections, records an RVA, and does not search
immediate/displacement bytes as if they were instruction boundaries. It also
refuses POPF/IRET, far transfers, segment loads and other system instructions
which could invalidate the recovery environment. IN/OUT remain admissible.

A linear disassembly is not a control-flow proof: code can branch into an
immediate, change executable memory, or issue raw port I/O without calling a
shim. The i686 direct map has no NX protection. In particular, admitting IN/OUT
cannot enforce BAR ownership on raw instructions. Do not present this scan or
same-privilege recovery as security isolation or sufficient admission evidence.

Microsoft's [hardware priority rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/managing-hardware-priorities)
and [spin-lock rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-spin-locks)
require explicit IRQL and real exclusion. The recovery mechanism must restore
the outer callback IRQL and release only locks registered by that adapter;
it must never guess that an unrelated kernel lock can be released. Untracked
host critical sections are excluded from recoverable shim execution.

The local read-only Windows 98 Resource Kit `wrkc16_1.htm` describes protected-mode
NDIS 3.1 networking, not the NT5 ABI. It does not change the selected NT5 policy.

## Runtime prerequisites still requiring lead qualification

* Registry hard-IRQ callbacks execute with IF clear. The current PIT cannot
  interrupt a spinning foreign ISR in that context. No thunk may enable IF
  behind the registry's back: source masking, nesting and accounting need review.
* The nominal 1 kHz PIT cannot enforce a 50 us ISR limit. ND6's 2 ms worker
  bound is not a passive callback budget; a passive budget needs a lead decision.
* A per-revision independent engine-stop/idle proof is still required. Clearing
  PCI command decode/BM and disabling INTx is a fence, not proof of completed DMA.
  Every uncertain image/DMA/context pin must remain allocated for the boot.
* Faults in unrelated host IRQ handlers or a marked host critical section must
  retain the existing host fault path. Arbitrary memory corruption is not
  recoverable merely because a foreign callback was active.

Host signal delivery can exercise the i386 saved-context unwind. It cannot
prove PIC nesting, ring-0 IRET, actual PCI fencing or continuation of the
desktop on hardware. Runtime qualification belongs to the lead.

## Staged code and host evidence

`execution.c` classifies recoverable trap frames, selects the outermost frame
at the same native IRQ nesting depth, and redirects IRET to the saved-context
trampoline in `arch/ndis_context.asm`. An inner IRQ frame must finish its native
EOI/accounting epilogue before an outer interrupted callback can be abandoned.
The task-local slot prevents attribution to a different task while a passive
shim sleeps. A nested host IRQ and explicitly marked host critical code remain
outside foreign fault attribution. Timeouts inside shims set a pending flag;
the last shim return performs the abandon. The callback state owns a bounded
list of pinned spin-lock words and the previous IRQL for cleanup.

The production `ndis_execute2` refuses unconditionally. Only
`CIUKI_NDIS_HOST_MODEL` builds contain its indirect foreign call. There is no
production runtime switch and no change to `ntimage_enter`'s closed gate.
The new native trap hooks are inert without a task's registered callback slot.

`fence.c` checks active registry generations, a common pinned owner and a BAR
of the selected Ethernet PCI function. The native backend serializes config
mechanism 1, checks vendor/device identity, clears command bits 0–2, sets bit
10 and verifies readback. It writes a 16-bit command only, preserving the W1C
status half. These fields are documented in Microsoft's
[PCI configuration structure](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_pci_common_config)
and the upstream [Linux PCI register definitions](https://github.com/torvalds/linux/blob/master/include/uapi/linux/pci_regs.h).
Exact-generation registry quarantine detaches the offending IRQ member;
peers' PIC masks are not changed by the fence. No engine-idle proof is supplied,
so the native fence always reports unproved idle and retains all pins.

The synthetic fixtures cover instruction/refusal offsets, bounded decoding,
IRQL and spin-lock recovery, permanent quarantine, PCI transactions with fake
I/O, trap classification, nested IRQ recovery targets and deferred shim expiry.
They include an accepted branch into a MOV immediate containing CLI/JMP: this
is a concrete counterexample to using a linear scan as proof against CLI spins.
The separate i386 signal harness contains actual PF/DE/UD, nested callbacks,
held locks and nonreturning ISR/DPC/passive/shim fixtures. In this sandbox it
compiles but exits with SIGSYS before it can establish execution evidence.
Native UBSan fixtures pass; ASan/UBSan fixtures reach their PASS markers but
fail at LeakSanitizer teardown because sandbox thread inspection is denied.
Leak detection remains enabled. See `build/codex/f6-05-report.md` for exact
commands, results and the part-2 sequencing refusal.
