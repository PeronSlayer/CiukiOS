# CiukiOS Protected-Mode Transition Contract (v1)

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
2. Full 16-bit to 32-bit protected-mode switch and DOS extender ABI emulation are pending.

## Verification Markers
1. Startup serial marker:
   - `[ compat ] PMODE contract v1 ready (CIUKEX64 marker + stub offset)`
2. MZ runtime dispatch marker in execution path:
   - `MZ dispatch (CIUKEX64): entry=...`

## Next Increment
1. Introduce explicit CPU mode transition state block.
2. Define register/segment preservation rules for extender bridge calls.
3. Add first protected-mode smoke harness for DOS/4GW-style loader expectations.
