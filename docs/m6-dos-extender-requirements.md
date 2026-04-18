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
	- `[m6] dpmi get-version callable slice ready`
	- `[m6] dpmi raw-mode bootstrap slice ready`
	- `[m6] rm callback skeleton ready`
	- `[m6] int reflect skeleton ready`
- Runtime slice now exposed through the services ABI with minimal `INT 2Fh AX=1687h` host-query support for smoke validation.
- Descriptor slice step-up now returns a non-zero host entry pointer (`ES:DI`) and host-data size (`SI`) so regressions can validate more than simple presence.
- First callable host slice is now exposed through the services ABI as `INT 31h AX=0400h` (Get Version), returning a DPMI 0.9-style host profile for client-side regression.
- First bootstrap-facing host slice is now exposed through the services ABI as `INT 31h AX=0306h` (Get Raw Mode Switch Addresses), returning non-zero entry points for the current smoke baseline.

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
5. **DPMI descriptor smoke:** `make test-m6-dpmi-smoke` validates a reproducible MZ smoke binary (`CIUKDPM.EXE` -> `0x49`) requiring non-zero descriptor metadata from `AX=1687h`
6. **DPMI callable smoke:** `make test-m6-dpmi-call-smoke` validates a reproducible MZ smoke binary (`CIUK31.EXE` -> `0x4B`) requiring `AX=1687h` descriptor metadata plus `INT 31h AX=0400h` success
7. **DPMI bootstrap smoke:** `make test-m6-dpmi-bootstrap-smoke` validates a reproducible MZ smoke binary (`CIUK306.EXE` -> `0x4E`) requiring `AX=1687h` descriptor metadata plus `INT 31h AX=0306h` success
8. **DPMI allocate-memory smoke:** `make test-m6-dpmi-mem-smoke` validates a reproducible MZ smoke binary (`CIUKMEM.EXE` -> `0x54`) requiring `AX=0501h` success with non-zero linear address and handle
9. **DPMI free-memory smoke:** `make test-m6-dpmi-free-smoke` validates a reproducible MZ smoke binary (`CIUKREL.EXE` -> `0x56`) requiring a stateful `AX=0501h` allocation followed by `AX=0502h` success and duplicate-free rejection
10. **DOOM packaging harness:** `make test-doom-target-packaging` validates deterministic image packaging/discovery for `DOOM.EXE`, `DOOM1.WAD`, `DEFAULT.CFG`, and `DOOM.BAT`
11. **Aggregate:** `bash scripts/test_doom_readiness_m6.sh`

## Acceptance Criteria

- `test_doom_readiness_m6.sh` PASS
- `test-m6-smoke` PASS
- `test-m6-dos4gw-smoke` PASS
- `test-m6-dpmi-smoke` PASS
- `test-m6-dpmi-call-smoke` PASS
- `test-m6-dpmi-bootstrap-smoke` PASS
- `test-m6-dpmi-mem-smoke` PASS
- `test-m6-dpmi-free-smoke` PASS
- `test-doom-target-packaging` PASS
- No regressions to existing INT21h, MZ runtime, or shell flow
- Real DOS/4GW execution remains next increment beyond the current descriptor + version + bootstrap + stateful memory callable baseline

## Reference

Defined in context of M6 milestone from roadmap-ciukios-doom.md.
