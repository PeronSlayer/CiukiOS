# CiukiOS - DOS 6.2 Compatibility Roadmap (Path 2)

Note:
For the current execution-oriented target (running DOS DOOM), see:
`docs/roadmap-ciukios-doom.md`
For FreeDOS symbiotic integration approach, see:
`docs/freedos-symbiotic-architecture.md`

## Goal
Recreate DOS 6.2 behavior with high compatibility at binary level (`.COM`, `.EXE MZ`) and API level (`INT 21h`), built from scratch.

## Current Snapshot (v0.6.1, updated 2026-04-17)
1. Stage2 baseline is stable with automated boot/fallback/FAT compatibility checks.
2. DOS-like shell commands for core file workflow are available.
3. COM execution path is active with PSP and termination lifecycle wiring.
4. EXE MZ path moved beyond MVP with relocation/edge-case hardening plus deterministic host-side regression suite.
5. INT21 priority-A subset includes file search and rename coverage (`4Eh/4Fh/56h`) and one-shot `AH=4Dh` status semantics.
6. Phase 2 low-level core exposes deterministic startup selftests for timer tick progress and keyboard decode/capture.
7. M6 planning and first execution gates are active (`docs/m6-dos-extender-requirements.md`, `make test-m6-pmode`, `scripts/test_doom_readiness_m6.sh`).

## Architecture Choice
1. Keep `UEFI x64` as modern bootstrap only.
2. Hand off to a dedicated `stage2` that prepares a DOS-compatible environment.
3. Build a DOS-like kernel/runtime focused on 16-bit style behavior and BIOS/DOS interrupt semantics.

Note: this is intentionally slower, but it is the best path to learn real DOS internals.

## Main Phases

| Phase | Focus | Minimum verifiable output |
|---|---|---|
| 0 | Compatibility foundation | Technical baseline, milestones, test plan, DOS-mode repo structure |
| 1 | Compatible bootstrap | UEFI loader -> stage2 -> controlled transition toward DOS-like runtime |
| 2 | DOS-like low-level core | IVT, PIC/PIT, keyboard, conventional memory map, base interrupts |
| 3 | Program loader | `.COM` loading, PSP, terminate/return code |
| 4 | File system | FAT12/16 read/write, base directory API |
| 5 | `INT 21h` core | Console, file I/O, memory, process lifecycle essentials |
| 6 | Compatible `COMMAND.COM` | Prompt, key internal commands, external program launch |
| 7 | `.EXE MZ` + batch | MZ loader, relocation, environment variables, `.BAT` parser |
| 8 | DOS configuration | `CONFIG.SYS`, `AUTOEXEC.BAT`, base device chain |
| 9 | Advanced memory | XMS/HMA/UMB (EMS as later extension) |
| 10 | Utilities + hardening | DOS-like utility set, regressions, stability and compatibility |

## Phase Details

### Phase 0 - Compatibility Foundation
Outputs:
- DOS API target map (priority A/B/C).
- Compatibility test plan (feature tests + binary tests).
- Directory scaffolding for `stage2` and DOS runtime.

Exit criteria:
- Reproducible build.
- Versioned compatibility document.
- Ordered backlog for phases 1-3.

### Phase 1 - Compatible Bootstrap
Outputs:
- Reliable handoff from UEFI to stage2.
- CPU state setup for entering DOS-like runtime.
- Serial/debug validation for each transition.

Exit criteria:
- Stable boot up to stage2 idle loop.
- No CPU exceptions during transition.

### Phase 2 - DOS-like Low-level Core
Outputs:
- Initialized IVT.
- Timer and keyboard ISRs.
- Base conventional memory management (simple allocator).

Exit criteria:
- Visible timer ticks.
- Stable keyboard input capture.

Status:
- Completed.

Completion evidence:
- Startup wiring initializes IDT, PIT/IRQ0, keyboard IRQ1 path, and enables interrupts in sequence.
- Boot serial log emits deterministic PASS markers:
	- `[ test ] phase2 timer tick progress: PASS`
	- `[ test ] phase2 keyboard decode/capture: PASS`
	- `[ test ] phase2 low-level core selftest: PASS`
- Boot gate validation checks now require those markers in `scripts/test_stage2_boot.sh`.
- Phase-2 closure gate exists and is reproducible via `make test-phase2`, combining:
	- `check_int21_matrix`
	- deterministic MZ regression (`test_mz_regression`)
	- real EXE corpus harness from FreeDOS/OpenGEM runtime (`test_mz_runtime_corpus`)

### Phase 3 - `.COM` Loader
Outputs:
- Minimal PSP.
- FAT-backed load/execute in target memory.
- `INT 20h`/`INT 21h AH=4Ch` termination path.

Exit criteria:
- At least 3 test `.COM` programs run successfully.

Status:
- Substantially completed.

Completion evidence:
- PSP-oriented COM runtime contract is active in stage2 shell runtime path.
- Lifecycle termination path (`INT 20h`, `INT 21h AH=4Ch`, `AH=4Dh`) has dedicated deterministic selftest coverage.

### Phase 4 - FAT12/16
Outputs:
- File and directory read/write.
- Cluster chain handling, 8.3 path parser.

Exit criteria:
- `DIR/TYPE/COPY/DEL` works on real files in disk image.

Status:
- Substantially completed (core subset).

Completion evidence:
- FAT-backed read/write and handle flows are active.
- E2E selftests cover handle create/open/read/write/seek/close/delete/rename and findfirst/findnext flows.

### Phase 5 - `INT 21h` Core
Outputs:
- Fundamental DOS APIs for console, files, memory, process management.

Exit criteria:
- API tests pass for priority-A subset.

Status:
- In progress (Priority-A baseline complete, hardening continues).

Completion evidence:
- Priority-A matrix gate exists and is green via `make check-int21-matrix`.
- Baseline, FAT-handle, and findfirst/findnext selftests are wired in startup flow.

Open work for closure:
- Deep DOS parity hardening (error-flag corner cases, memory ownership metadata, and broader compatibility traces).

### Phase 6 - `COMMAND.COM`
Outputs:
- Prompt and base command interpreter.
- Essential internal commands.

Exit criteria:
- Full interactive session without crashes.

### Phase 7 - `.EXE MZ` + Batch
Outputs:
- MZ loader with relocation.
- `.BAT` parser with base control flow.

Exit criteria:
- Simple MZ program runs correctly.
- Real batch script executes.

Status:
- In progress (MZ advanced, batch pending).

Completion evidence:
- MZ parser/loader now includes relocation and boundary hardening with deterministic regression coverage.

Open work for closure:
- Real `.BAT` parser/control-flow pipeline and env expansion behavior.

### Phase 8 - DOS Config
Outputs:
- Boot configuration from `CONFIG.SYS` and `AUTOEXEC.BAT`.
- Minimal device chain.

Exit criteria:
- Automatic startup sequence works.

### Phase 9 - Advanced Memory
Outputs:
- XMS/HMA/UMB interface.

Exit criteria:
- Extended-memory allocation demo from utility test.

### Phase 10 - Utilities + Hardening
Outputs:
- Main DOS-like utilities.
- Regression suite and compatibility report.

Exit criteria:
- Release candidate with stable test results.

## How We Work Together (Teaching Mode)
For each change set, I always explain in 3 points:
1. `What` we change.
2. `Why` we change it now.
3. `How` we test it immediately.

Then we code, run tests, and close with a short recap.
