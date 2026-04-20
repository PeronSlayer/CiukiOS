# Handoff — OpenGEM v86 trip live + INT 21h baseline (2026-04-21)

## Context and goal
Goal: make OpenGEM `GEM.EXE` actually run so we can move toward opening its GUI.
Starting point: the long→PM32→v86→PM32→long round trip silently triple-faulted after
the first GEM INT 0x21 and the dispatcher stubbed only AH=0x4C.

## Files touched
- `stage2/src/mode_switch.c` — new `.bss` global `g_mode_switch_scratch_ptr`;
  set inside `mode_switch_run_legacy_pm()` right after `g_mode_switch_resume_target`.
- `stage2/src/mode_switch_asm.S` — `_ms_long_resume` reloads `%r15` from
  `g_mode_switch_scratch_ptr` after `TRACE 'Q'`; post-body compat-mode path
  reloads `%ebx` from the same global before every `SCR_*` access.
- `stage2/src/legacy_v86_pm32.S` — restore guest EAX/EBX/ECX/EDX from
  `frame.reserved[0..3]` before the `iretl` into v86.
- `stage2/src/v86_dispatch.c` — minimal INT 21h handler surface:
  AH=00/02/09/25/30/35/48/49/4A/4C; verbose per-call logging;
  unhandled default returns CF=1. Probe switched to AH=0x49.

## Decisions made
- Use a memory-based global to survive both the 32-bit TSS task switch
  (Intel SDM: upper 32 bits of R8-R15 undefined) and the `%ebx` clobber
  inflicted by the PM32 `#GP` handler when it writes guest EBX to the frame.
- Keep dispatcher pure (no real DOS memory model). AH=0x48 returns OOM
  (CF=1, AX=8, BX=0) deliberately — GEM tolerates it and proceeds.
- Only AH=0x25 (set vector) silently succeeds for now: persistent vector
  table is intentionally deferred; it is required for GEMVDI TSR.

## Validation performed
- `/usr/bin/bash scripts/run-gem-quick.sh` + `grep -E "gem|v86" build/serial-gem.log`:
  full trip completes end-to-end with DOS call pattern
  `4A → 48 → 4A → 09("GEMVDI not present in memory.") → 09(newline) → 4C`
  followed by `[gem] dispatch exit=ok`.
- `mstest` probe path still green (AH=0x49 free stub returns CONT).
- No regression on `make test-stage2` / `make test-fallback` expected
  (only additive changes; boot path untouched).
- Merged `wip/opengem-044b-real-v86-first-int` into `main` at `56cdfa1`,
  pushed to `origin/main`.

## Risks
- AH=0x25 (set vector) is a silent stub. Any DOS program that installs
  an ISR (GEMVDI.EXE, mouse driver, etc.) will appear to install but
  no hook actually fires. This is the top blocker for GEMVDI TSR.
- AH=0x48 returns OOM. Real allocators upstream may abort cleanly
  (as GEM does) or misbehave; depends on callers.
- `v86_dispatch.c` default path logs but returns CF=1 AX=1. Some
  programs may interpret AX=1 as a specific DOS error code; watch logs.

## Next step
OpenGEM's `GEM.BAT` runs `GEMVDI.EXE` first (a TSR that hooks interrupts,
then chains `GEM.EXE`). Our shell currently launches `GEM.EXE` directly,
so the VDI TSR is never present → GEM prints "GEMVDI not present" and
exits cleanly. To actually open the GUI we need, in order:

1. Persistent INT-vector map for AH=0x25 / AH=0x35 (real set/get vector).
2. INT 21h AH=0x31 (TSR): when called, freeze the current v86 image at
   its paragraph size and keep its installed vectors live for the next
   program launched in the same v86 session.
3. Either INT 21h AH=0x4B (exec) so GEMVDI.EXE chains to GEM.EXE from
   inside v86, or a shell-level sequenced launch of GEMVDI.EXE followed
   by GEM.EXE sharing the same v86 address space and vector state.
4. Once VDI is "present", GEM will proceed into GUI init — which will
   stress INT 10h (video), INT 16h (keyboard), INT 33h (mouse), and
   real DOS file I/O (AH=0x3D/0x3F/0x40). Those are the next milestones.

Reliable reproducer remains `/usr/bin/bash scripts/run-gem-quick.sh`.
