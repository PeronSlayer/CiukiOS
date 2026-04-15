# Phase 0 - Operational Kickoff

## Current Phase Goal
Prepare the project to start the high-compatibility DOS path without architectural ambiguity.

## Tasks (Execution Order)
1. Define directory structure for `stage2` and DOS runtime.
2. Define `UEFI -> stage2` handoff ABI.
3. Build minimal boot tests for each CPU transition step.
4. Define first `INT 21h` API list (priority A) to implement.
5. Prepare first `.COM` test programs.

## Concrete Deliverables
1. ABI document with structs and preserved registers.
2. Bootable `stage2` draft (banner + halt is enough).
3. Test script with pass/fail boot output.
4. Priority-A API table.

## Definition of Done
1. Full boot to stage2 with no exceptions.
2. Clear debug output for each checkpoint.
3. Phase-1 backlog ready with small tasks (1-3 hours each).

## First Iteration (Next Step)
In the next step we implement:
1. `stage2` scaffolding.
2. Minimal handoff from current loader.
3. QEMU test with serial/debug checkpoints.

## Progress
Iteration 1 completed:
1. `stage2` scaffolding created and bootable.
2. Loader -> stage2 handoff working with ABI v0.
3. Positive QEMU boot test with serial checkpoints.

Iteration 2 completed:
1. Automated `stage2` test with required-marker validation.
2. `make test-stage2` target for quick execution.

Iteration 3 completed:
1. Automated fallback test (`stage2` missing -> `kernel.elf`).
2. `make test-fallback` and aggregate `make test-boot` targets.

Iteration 4 completed:
1. Stage2 local IDT scaffolding with dedicated default interrupt stub.
2. Stage2 boot test now validates IDT activation checkpoint.

Iteration 5 completed:
1. Stage2 local GDT/TSS ownership enabled (`lgdt + ltr` + segment reload).
2. Dedicated exception stubs for `#UD/#GP/#PF` with deterministic serial markers.
3. PIC remap + PIT setup + IRQ0 tick path validated by automated boot test.

Iteration 6 completed:
1. IRQ1 keyboard path enabled (IDT vector `0x21`) with scancode logger.
2. PIC master mask updated to unmask both IRQ0 and IRQ1.
3. Stage2 boot test updated with keyboard-ready checkpoint.
