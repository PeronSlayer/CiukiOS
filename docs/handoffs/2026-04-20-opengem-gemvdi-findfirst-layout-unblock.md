# Handoff — OpenGEM GEMVDI findfirst/layout unblock + post-match loop progress

## 1) Context and goal
Goal of this session was to unblock GEMVDI on CiukiOS by addressing the known hard stop on `INT 21h AH=4E` (`AX=0x12` no-match), while staying on branch `wip/opengem-044b-real-v86-first-int` and preserving IA-32 / legacy-BIOS project invariants.

Starting point from previous handoff:
- GEMVDI reached the expected probe sequence.
- All probes failed at file search (`AH=4E/4F` always no-match).
- Runtime then printed `No screen driver found` and exited.

## 2) Files touched
- `stage2/src/v86_dispatch.c`
- `stage2/src/legacy_v86_pm32.S`
- `run_ciukios.sh`

## 3) Decisions made
1. **Implemented path B in runtime first** (correctness-first):
   - Real FAT-backed wildcard search wired for `AH=4E/4F`.
   - Guest-side current directory tracking added (`AH=3B` updates cwd, root `/` baseline).
   - DTA handling stabilized around `g_v86_dta_linear` + 43-byte DOS record fill.
   - `AH=47` kept with existing DS:DX compatibility approximation (SI unavailable in current frame ABI).

2. **Applied path A as pragmatic unblock in image packaging**:
   - During image build, stage `SDPSC9.VGA` to root (`::SDPSC9.VGA`).
   - Stage `GEM.EXE` to `::GEMBOOT/GEM.EXE`.
   - This matches the real GEMVDI search contract observed in serial traces.

3. **Fixed post-match trap instability in PM32 handler**:
   - In `legacy_v86_pm32.S` #GP handler, recompute PIC base every trap entry.
   - Rationale: guest code may modify `BP`; relying on inherited `%ebp` for handler-relative addressing is unsafe.

4. **Removed next loop blocker after successful driver scan**:
   - Added minimal `AH=4B` stub (EXEC success path + serial path log).
   - Added minimal `AH=08` deterministic input surrogate to avoid looping on unhandled input call.

## 4) Validation performed
Commands run repeatedly during iteration:
1. `touch stage2/src/shell.c && make build/stage2.elf`
2. `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios.sh`
3. `bash scripts/run-gemvdi-probe.sh`
4. focused log filters with `rg`/`grep` on `build/serial-gem.log` and `build/qemu.log`

Key runtime evidence from `build/serial-gem.log` after fixes:
- `AH=4E` now finds files:
  - `pattern="GEM.EXE"` + `match name=GEM.EXE dir=/GEMBOOT`
  - `pattern="SD*.*"` + `match name=SDPSC9.VGA dir=/`
  - subsequent probes: `VD*.*`, `PD*.*`, `MD*.*`, `CD*.*`, `ID*.*`
- `AH=4B` reached and handled:
  - `int21/4B exec path="GEM.EXE"`
  - `int21/4B canonical=/GEMBOOT/GEM.EXE`
- sequence now exits cleanly with:
  - `[gem] dispatch exit=ok`

Observed removed failure signatures:
- no longer blocked on `AH=4E` no-match at `SD*.*`.
- no `No screen driver found` in the validated post-fix probe path.

## 5) Risks and next step
Current `AH=4B` is a success stub (no real child load/execute chain yet), so GUI opening is not yet guaranteed by this session alone.

Most direct next steps:
1. Implement real `AH=4B` execute semantics (or explicit shell-level chain) so GEMVDI can actually transfer control to GEM.EXE in-process.
2. Then wire `AH=3D/3F/40/3E` only if traces show GEMVDI or chained GEM.EXE still depends on them in this path.
3. Validate with two probes:
   - `gem vdi` (TSR-first path)
   - `gem` (main launcher path)

No version bump performed (stays `Alpha v0.8.9`).
