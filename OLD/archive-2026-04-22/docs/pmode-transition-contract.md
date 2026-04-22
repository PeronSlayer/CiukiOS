# CiukiOS Protected-Mode Transition Contract (v2 Baseline)

## Goal
Define a deterministic handoff contract between stage2 DOS runtime and future DOS extender-compatible 32-bit execution path.

## Marker Contract
1. MZ image dispatch marker is `CIUKEX64` at module offset `0x0000`.
2. Stub entry offset is stored as little-endian `u32` at module offsets `0x0008..0x000B`.
3. Stage2 validates marker and bounds before dispatch.

## Runtime Expectations (v1)
1. Stage2 loads MZ image and applies relocations to DOS load segment.
2. Stage2 validates runtime span and entry offsets before transfer.
3. Stage2 publishes process lifecycle through INT21-compatible exit status path (`AH=4Dh`).

## Current Status
1. Contract markers and dispatch validation are implemented in stage2 runtime path.
2. Transition-state baseline contract is implemented with deterministic startup markers.
3. Full 16-bit to 32-bit protected-mode switch and DOS extender ABI emulation are pending.

## Verification Markers
1. Startup serial marker:
   - `[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)`
2. Transition baseline markers:
   - `[m6] transition state init: PASS`
   - `[m6] gdt/idt snapshot: PASS`
   - `[m6] cr0 transition contract: PASS`
   - `[m6] return-path contract: PASS`
3. Entry infrastructure markers:
   - `[m6] a20 probe=on|off`
   - `[m6] a20 enable result=PASS|FAIL`
   - `[m6] descriptor baseline ready=1`
4. Host skeleton markers:
   - `[m6] dpmi detect skeleton ready`
   - `[m6] rm callback skeleton ready`
   - `[m6] int reflect skeleton ready`
5. Pmode memory accounting markers:
   - `[m6] pmem range base=0x... size=0x...`
   - `[m6] pmem overlap check: PASS`
6. MZ runtime dispatch marker in execution path:
   - `MZ dispatch (CIUKEX64): entry=...`

## Next Increment
1. Replace skeleton host interface with first callable DPMI/reflect implementation slice.
2. Add first real DOS/4GW smoke binary harness.
3. Expand transition return-path validation beyond baseline marker contract.
