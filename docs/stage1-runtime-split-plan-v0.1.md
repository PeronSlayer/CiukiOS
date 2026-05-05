# Stage1 Runtime Split Plan v0.1

## Current Objective
Move CiukiOS away from byte-by-byte Stage1 growth by establishing a durable loader-plus-runtime architecture while preserving current full and full-CD behavior.

The governing rule is: Stage1 is a loader, not the operating system. New runtime features should live in a loaded runtime, shell component, helper, driver, or service unless they are required to locate and load the next runtime component.

## Current Stage1 Size Baseline
The active Stage1 slot is 70 sectors, or 35,840 bytes.

| Profile | Build mode | Size source | Margin source | Notes |
|---|---|---|---|---|
| full | FAT16 HDD, C: default | 35,476 bytes | 364 bytes | Stage1 remains the current DOS runtime owner; the first slice adds no Stage1 code. |
| full-cd | FAT16 Live/install CD, D: default | 35,541 bytes | 299 bytes | Critical profile because CD-specific defines keep the margin tighter than full. |

The first migration slice intentionally does not add Stage1 code. It creates an external runtime artifact and packages it under `\SYSTEM` so later slices have a concrete landing zone without increasing Stage1 size.

## Stage1 Responsibility Inventory

| Class | Current responsibilities | Split direction |
|---|---|---|
| Boot/load mandatory | Segment setup, stack, boot drive state, BIOS disk reads/writes, LBA/CHS fallback, FAT geometry constants, Stage1 slot execution. | Keep in Stage1. |
| FAT/file loading before runtime | Root/subdirectory lookup, FAT cache, 8.3 path parsing, cluster walking, file-sector reads needed to find boot payloads. | Keep only the minimum loader subset in Stage1; move general DOS file semantics to runtime. |
| DOS runtime core | INT 20h/21h/2Fh/10h/15h hooks, PSP, MCB allocator, DTA, handles, COM/MZ execution, file APIs, XMS compatibility. | Move to `\SYSTEM\RUNTIME.BIN` after an ABI and state handoff are defined. |
| Shell/UX | Prompt, line editor, command dispatch, built-ins, history/completion, footer telemetry. | Move to `\SYSTEM\SHELL.COM` or a runtime-owned shell component. |
| Diagnostics/debug | Serial markers, IERR logs, hardware validation screens, selftest markers, diagnostic commands. | Gate debug-only output or move test helpers outside default Stage1. |
| Driver/CD support | Mouse/INT33, PS/2 paths, stage2 services, driver helper assumptions, CD/MSCDEX compatibility surfaces. | Move policy to runtime/modules; keep only boot-critical media detection in Stage1. |
| Graphics/demo/support | Splash loader/blitter, VDI demo primitives, glyph/demo support. | Keep only user-approved boot splash pieces in Stage1; move demos/helpers out. |
| Test-only/selftest | Stage1 selftest, file/path smoke helpers, demo launch tests, serial test strings. | Gate out of default builds or move to standalone validation helpers. |

## Target Architecture

```text
Boot sector
  -> minimal Stage1 loader
      -> load \SYSTEM\RUNTIME.BIN
          -> initialize DOS runtime and compatibility services
          -> start \SYSTEM\SHELL.COM or runtime shell entry
          -> load optional drivers/services/modules
```

## Boundaries
Stage1 after the split should establish real-mode execution, read enough FAT16 to locate `\SYSTEM\RUNTIME.BIN`, load the runtime into a documented segment, transfer through a small ABI structure, and provide a minimal fatal error if loading fails.

The runtime binary should own DOS compatibility interrupts, PSP/MCB state, DTA, handles, file APIs, COM/MZ execution, XMS, driver/CD compatibility, runtime diagnostics, and shell startup.

The shell should not remain permanently linked into Stage1. The preferred later boundary is `\SYSTEM\SHELL.COM` once the loaded runtime owns enough DOS APIs to launch it.

Driver policy, CD/MSCDEX activation, setup helpers, demos, and validation helpers should become runtime modules, `.COM` helpers, or files under `\SYSTEM` and `\APPS`, not Stage1 features.

## Load Path And File Names
Initial landing file: `\SYSTEM\RUNTIME.BIN`.

Future candidates:
1. `\SYSTEM\KERNEL.BIN` if the loaded component becomes the primary kernel image.
2. `\SYSTEM\SHELL.COM` for shell extraction.
3. `\SYSTEM\MODULES\*.BIN` for optional runtime services after module policy exists.

## Memory Layout And ABI Assumptions
The first external artifact is inert and not executed. Future executable runtime slices should use a fixed load segment chosen to avoid Stage1, FAT buffers, DOS heap, application load regions, and full-CD setup paths. A small handoff structure should include boot drive, default DOS drive, FAT geometry, runtime load segment, and feature flags.

The first executable runtime should be a flat binary with a fixed segment ABI. Relocation should be deferred until a flat load/entry path is proven in full and full-CD.

## Error And Fallback Behavior
Default full/full-CD boot behavior must remain unchanged until an owner-approved runtime handoff exists. Any opt-in runtime probe must fail closed to the current Stage1 path and must avoid noisy permanent traces.

## First Safe Slice Implemented
This cycle implements the safest structural slice:

1. Add `src/runtime/runtime.asm` as a tiny inert runtime placeholder with signature `CIUKRT01`.
2. Build it as `build/full/obj/runtime.bin`.
3. Package it as `\SYSTEM\RUNTIME.BIN` in the full image.
4. Let full-CD inherit the artifact through `scripts/build_full_cd.sh`, which delegates to `scripts/build_full.sh`.
5. Do not modify Stage1 boot flow or user-visible behavior.

This does not reduce Stage1 bytes yet. It makes the split real and testable without adding Stage1 risk.

## Migration Order
1. Establish external runtime artifact packaging and validation.
2. Add an opt-in Stage1 runtime load probe only after freeing enough Stage1 bytes or gating debug code.
3. Move diagnostics/debug-only code out of default Stage1 or behind release/debug defines.
4. Move shell/UX into `\SYSTEM\SHELL.COM` after runtime launch ABI is stable.
5. Move DOS runtime services from Stage1 into `\SYSTEM\RUNTIME.BIN` in small, tested groups: memory, file/path, process, XMS, driver/CD.
6. Move driver/CD policy into runtime/module layer.
7. Keep Stage1 as loader plus minimal emergency fallback.

## Acceptance Gates
Every migration phase must pass active profile checks only:

1. `make build-full`
2. `make build-full-cd`
3. `make qemu-test-full`
4. `make qemu-test-full-cd`
5. `make qemu-test-full-cd-shell-drive`
6. `make qemu-test-full-shell-stability`
7. `make qemu-test-full-drvload-smoke`
8. `make qemu-test-all`
9. `DOOM_TAXONOMY_MIN_STAGE=runtime_stable make qemu-test-full-doom-taxonomy` when local DOOM assets are present.

No floppy, FAT32, GUI expansion, UX change, merge, or push is part of this plan without explicit owner approval.

## Executable Probe Slice Implemented
The completion slice moves the split beyond an inert artifact while preserving default behavior.

### Stage1 Size Recovery
Selftest-only code and data are now gated behind `STAGE1_SELFTEST_AUTORUN`. Default full and full-CD builds no longer carry the Stage1 autorun move/rename test, stream-C resolver/footer selftest, selftest orchestrator, or their private strings. Selftest builds still include the same code because `scripts/qemu_test_full_stage1.sh` exports `CIUKIOS_STAGE1_SELFTEST_AUTORUN=1`.

| Profile | Before | After | Bytes recovered | Free margin |
|---|---:|---:|---:|---:|
| full default | 35,476 | 34,949 | 527 | 891 |
| full-cd default | 35,541 | 35,014 | 527 | 826 |
| full runtime probe | n/a | 35,164 | n/a | 676 |

The full-CD margin now exceeds the 512-byte minimum target. The 1,024-byte preferred target remains a follow-up extraction goal.

### Runtime Load Probe
Probe flag: `CIUKIOS_STAGE1_RUNTIME_PROBE=1`, passed to NASM as `STAGE1_RUNTIME_PROBE`.

When enabled, Stage1 opens `\SYSTEM\RUNTIME.BIN`, loads up to 512 bytes at segment `0x4C00`, verifies signature `CIUKRT01` at offset `0x0002`, calls `0x4C00:0x0000`, then falls back to the existing boot/shell path. Default builds leave the probe compiled out.

Minimal probe serial markers are gated with the probe:
1. `[RTP] B` when the probe starts.
2. `[RTP] OK` after signature verification, runtime call, and ABI status validation.
3. `[RTP] BAD` when load/signature/ABI validation fails; boot continues through the existing fallback path.

### Runtime Handoff ABI
Stage1 passes `ES:DI` pointing to a small handoff/status buffer in Stage1 data before far-calling the runtime.

| Offset | Size | Owner | Meaning |
|---:|---:|---|---|
| `0x00` | word | runtime writes | ABI version, currently `1`. |
| `0x02` | word | runtime writes | Runtime service count, currently `1`. |
| `0x04` | word | runtime writes | Runtime status flags, bit 0 currently means identity/status service ready. |

This delegates the first real responsibility to the loaded runtime: runtime identity/service status is produced by `RUNTIME.BIN` and consumed by the probe lane before `[RTP] OK` is emitted.

### Next Migration Slice
Move a tiny runtime-owned service table header into `RUNTIME.BIN` and make the probe query a callable service descriptor rather than only fixed status words. Keep it opt-in, keep default boot independent of `RUNTIME.BIN`, and only then consider extracting a diagnostic or shell constant.
