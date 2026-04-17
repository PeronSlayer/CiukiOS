# M6: Protected-Mode and DOS Extender Readiness

**Milestone Focus:** Establish the protected-mode transition contract required by DOS extenders (DOS/4GW class).

## Required Capabilities

### 1. Real-Mode Entry Point
- A20 gate control and state verification
- GDT/IDT state preservation across mode transitions  
- Interrupt vector table (IVT) baseline (at least stubs for critical vectors)

### 2. Protected-Mode Transition
- `LGDT` + `LIDT` + initial CR0 setup
- Initial mode establishment (no paging initially; identity mapping sufficient)
- Register preservation: `CS:IP`, stack pointer, callee-saved registers
- Return-to-real-mode path validation

### 3. DOS Extender Interface
- DPMI host detect mechanism (minimal DOS/4GW host mode query)
- Real-mode callback setup for protected-mode drivers
- Interrupt reflection (from pmode to real-mode handler) baseline

### 4. Memory Accounting
- Track allocated pmode memory (separate from DOS conventional/HMA)
- Prevent overlap with existing stage2 code/data
- Validate extender load assumptions (typically 1MB+ available)

## Test Gates

1. **Static:** Verify IDT, GDT, CR0 macros/constants exist in kernel
2. **Runtime:** Boot message shows "pmode-transition: ready" or failure reason
3. **Integration:** Real DOS/4GW app can call into pmode without immediate crash

## Acceptance Criteria

- `test_doom_readiness_m6.sh` PASS
- No regressions to existing INT21h, MZ runtime, or shell flow
- At least one DOS/4GW app (e.g., simple extender utility) reaches pmode handler

## Reference

Defined in context of M6 milestone from roadmap-ciukios-doom.md.
