# CiukiOS - DOS 6.2 Compatibility Roadmap (Path 2)

Note:
For the current execution-oriented target (running DOS DOOM), see:
`docs/roadmap-ciukios-doom.md`

## Goal
Recreate DOS 6.2 behavior with high compatibility at binary level (`.COM`, `.EXE MZ`) and API level (`INT 21h`), built from scratch.

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

### Phase 3 - `.COM` Loader
Outputs:
- Minimal PSP.
- FAT-backed load/execute in target memory.
- `INT 20h`/`INT 21h AH=4Ch` termination path.

Exit criteria:
- At least 3 test `.COM` programs run successfully.

### Phase 4 - FAT12/16
Outputs:
- File and directory read/write.
- Cluster chain handling, 8.3 path parser.

Exit criteria:
- `DIR/TYPE/COPY/DEL` works on real files in disk image.

### Phase 5 - `INT 21h` Core
Outputs:
- Fundamental DOS APIs for console, files, memory, process management.

Exit criteria:
- API tests pass for priority-A subset.

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
