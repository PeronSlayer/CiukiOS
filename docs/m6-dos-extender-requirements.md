# M6: Protected-Mode and DOS Extender Readiness

**Milestone Focus:** Establish the protected-mode transition contract required by DOS extenders (DOS/4GW class).

## Required Capabilities

### 1. Real-Mode Entry Point
- A20 gate control and state verification
- GDT/IDT state preservation across mode transitions  
- Interrupt vector table (IVT) baseline (at least stubs for critical vectors)
- Baseline markers implemented:
	- `[m6] a20 probe=on|off`
	- `[m6] a20 enable result=PASS|FAIL`
	- `[m6] descriptor baseline ready=1`

### 2. Protected-Mode Transition
- `LGDT` + `LIDT` + initial CR0 setup
- Initial mode establishment (no paging initially; identity mapping sufficient)
- Register preservation: `CS:IP`, stack pointer, callee-saved registers
- Return-to-real-mode path validation
- Baseline transition contract v2 markers:
	- `[m6] transition state init: PASS`
	- `[m6] gdt/idt snapshot: PASS`
	- `[m6] cr0 transition contract: PASS`
	- `[m6] return-path contract: PASS`

### 3. DOS Extender Interface
- DPMI host detect mechanism (minimal DOS/4GW host mode query)
- Real-mode callback setup for protected-mode drivers
- Interrupt reflection (from pmode to real-mode handler) baseline
- Skeleton markers implemented:
	- `[m6] dpmi detect skeleton ready`
	- `[m6] rm callback skeleton ready`
	- `[m6] int reflect skeleton ready`
- Runtime slice now exposed through the services ABI with minimal `INT 2Fh AX=1687h` host-query support for smoke validation.

### 4. Memory Accounting
- Track allocated pmode memory (separate from DOS conventional/HMA)
- Prevent overlap with existing stage2 code/data
- Validate extender load assumptions (typically 1MB+ available)
- Baseline markers implemented:
	- `[m6] pmem range base=0x... size=0x...`
	- `[m6] pmem overlap check: PASS`

## Test Gates

1. **Runtime baseline:** `make test-m6-pmode` validates M6 contract + skeleton markers
2. **Transition v2 runtime:** `bash scripts/test_m6_transition_contract_v2.sh`
3. **Smoke executable:** `make test-m6-smoke` validates a reproducible MZ smoke binary (`CIUKPM.EXE` -> `0x36`) included in the OS image
4. **DOS/4GW-like smoke:** `make test-m6-dos4gw-smoke` validates a reproducible MZ smoke binary (`CIUK4GW.EXE` -> `0x47`) calling the minimal DPMI host query path
5. **Aggregate:** `bash scripts/test_doom_readiness_m6.sh`

## Acceptance Criteria

- `test_doom_readiness_m6.sh` PASS
- `test-m6-smoke` PASS
- `test-m6-dos4gw-smoke` PASS
- No regressions to existing INT21h, MZ runtime, or shell flow
- Real DOS/4GW execution remains next increment beyond skeleton baseline

## Reference

Defined in context of M6 milestone from roadmap-ciukios-doom.md.
