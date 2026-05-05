# Stage1 Runtime Split Plan v0.1

## Current Objective
Move CiukiOS away from byte-by-byte Stage1 growth by establishing a durable loader-plus-runtime architecture while preserving current full and full-CD behavior.

The governing rule is: Stage1 is a loader, not the operating system. New runtime features should live in a loaded runtime, shell component, helper, driver, or service unless they are required to locate and load the next runtime component.

## Current Stage1 Size Baseline
The active Stage1 slot is 70 sectors, or 35,840 bytes.

| Profile | Build mode | Size source | Margin source | Notes |
|---|---|---|---|---|
| full | FAT16 HDD, C: default | 34,989 bytes | 851 bytes | Current validated default Stage1 after the five-service runtime tranche. |
| full-cd | FAT16 Live/install CD, D: default | 35,054 bytes | 786 bytes | Tightest active profile; default boot still stays within the 70-sector ceiling. |
| full runtime probe | FAT16 HDD, probe-enabled validation build | 35,336 bytes | 504 bytes | Probe success now requires runtime services `1` through `5` plus default-drive consistency. |

Current `RUNTIME.BIN` size is 167 bytes. The split is now beyond the inert landing slice: the runtime owns a validated five-service table, while default full and full-CD boots still preserve Stage1 fallback ownership whenever runtime bootstrap or validation fails.

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
| full runtime probe | n/a | 35,174 | n/a | 666 |

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

## Service Table Foundation Finalized
This cycle closes the architecture foundation for the Stage1/runtime split without changing default boot ownership.

### Additional Stage1 Margin Recovery
A dead Stage1 DOS-memory helper cluster was removed from default builds after confirming the symbols had no remaining references. This recovered enough space to exceed the preferred 1,024-byte full-CD margin target.

| Profile | Before | After | Bytes recovered this cycle | Free margin |
|---|---:|---:|---:|---:|
| full default | 34,949 | 34,421 | 528 | 1,419 |
| full-cd default | 35,014 | 34,486 | 528 | 1,354 |
| full runtime probe | 35,174 | 34,768 | 406 | 1,072 |

The default full-CD build now clears the preferred margin target while keeping default boot and shell behavior unchanged.

### Runtime-Owned Service Table
The probe handoff no longer exposes ABI version and service count as fixed Stage1-owned status words. Runtime entry now writes a pointer to a runtime-owned service table plus a status flag word through `ES:DI`.

Handoff buffer written by runtime entry:
| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | word | Service table offset inside `RUNTIME.BIN`. |
| `0x02` | word | Service table segment, currently `0x4C00`. |
| `0x04` | word | Runtime status flags; bit 0 means runtime services ready. |

Runtime service table layout at the returned far pointer:
| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | dword | Header magic `RTSV`. |
| `0x04` | word | ABI version, currently `1`. |
| `0x06` | word | Service count, currently `5`. |
| `0x08` | word | Descriptor size, currently `8`. |
| `0x0A` | 8 bytes | First service descriptor. |
| `0x12` | 8 bytes | Second service descriptor. |
| `0x1A` | 8 bytes | Third service descriptor. |
| `0x22` | 8 bytes | Fourth service descriptor. |
| `0x2A` | 8 bytes | Fifth service descriptor. |

Runtime service descriptor format:
| Offset | Size | Meaning |
|---:|---:|---|
| `0x00` | word | Service id. |
| `0x02` | word | Flags, bit 0 means callable. |
| `0x04` | word | Far-call entry offset in the runtime segment. |
| `0x06` | word | Reserved. |

1. Service id `1` remains the runtime identity/status service. It has no side effects, returns `CF=0`, and returns `AX=0x5254`.
2. Service id `2` is a version-string diagnostic provider. It returns `CF=0` and returns `DS:SI` pointing at the runtime-owned `runtime_version` string.
3. Service id `3` is a stage2-ready marker provider. It returns `CF=0` and returns `DS:SI` pointing at a runtime-owned `[S2]` ready marker string.
4. Service id `4` is a DOS version query provider. It returns `CF=0` and provides the runtime-owned DOS version result used by the normal full/full-CD silent bootstrap cache and by `INT 21h AH=30h` forwarding when runtime state is available.
5. Service id `5` is a default-drive state bridge. It returns `CF=0` and `DS:SI` pointing at the runtime-owned default-drive byte.

Normal full and full-CD boots now perform a silent runtime bootstrap/cache step so Stage1 can consult runtime-owned immutable and low-risk live state without changing visible boot output. `INT 21h AH=30h` forwards to cached service id `4` state when available and falls back to the previous local DOS version path when runtime bootstrap or the service call is unavailable.

Stage1 now also uses service id `5` as the narrow mutable-state bridge for the current tranche. Stage1 reads the runtime-owned default-drive byte when available, synchronizes the runtime-owned byte after silent runtime init, and mirrors later set-default-drive updates back into runtime state so the current default-drive view remains aligned without making normal boot depend on `RUNTIME.BIN`.

The first live extraction also now consumes runtime service id `3` inside `init_stage2_services`. If the runtime marker is unavailable or invalid, Stage1 keeps the previous local Stage2-ready behavior, including the preserved fallback-local serial marker path.

Stage1 now requires a valid header, five valid callable descriptors, a successful service id `1` call, a successful service id `2` call returning the expected `CiukiOS runtime split` prefix from runtime-owned memory, a successful service id `3` call returning the runtime-owned `[S2]` ready marker string, a successful service id `4` call returning the expected DOS version state, a successful service id `5` call returning a default-drive pointer inside the runtime segment, matching Stage1/runtime default-drive bytes, and `status_flags & 1` before emitting probe success.

### Current Validated Five-Service Sizes
| Artifact | Size |
|---|---:|
| full Stage1 | 34,989 bytes |
| full-cd Stage1 | 35,054 bytes |
| full runtime probe Stage1 | 35,336 bytes |
| `RUNTIME.BIN` | 167 bytes |

### Probe Validation Semantics
Probe markers now represent ordered checkpoints:
1. `[RTP] B` - runtime probe started.
2. `[RTP] T` - runtime service table header and descriptor validated.
3. `[RTP] C` - runtime service calls returned the expected identity, version string, stage2-ready marker, DOS version state, and default-drive pointer/consistency results.
4. `[RTP] OK` - probe success after all validations.
5. `[RTP] BAD` - runtime load, signature, table, descriptor, or service validation failed; fallback continued safely.

### Service id `5` Completed
This cycle completed the previous conservative extraction target by moving default-drive state into a runtime-owned byte exposed through a service-table pointer, while still keeping Stage1 as the visible default owner when runtime bootstrap or validation fails.

### Conservative Next Extraction Target
Keep the next slice narrower than a subsystem transfer: extract one more read-mostly runtime-owned profile or diagnostic byte that is consumed by exactly one existing Stage1 path and still has an unchanged local fallback. Do not move allocator, PSP/MCB, EXEC, file-I/O core, termination, or CD/device-mutation ownership yet.

## Post-Foundation Product Order
After the current split foundation, the execution order should remain conservative:

1. Continue the runtime split with the next read-only immutable-state service and other tiny runtime-owned slices.
2. Add one read-only runtime service that returns real runtime or profile state.
3. Move one small live helper or diagnostic path into runtime ownership.
4. Use the stronger runtime boundary to improve arbitrary DOS program compatibility from the full and full-CD profiles.
5. Treat legacy audio as the next major compatibility milestone after broader DOS application bring-up.
6. Keep networking and Windows pre-NT behind those priorities.

The split is therefore not an isolated architecture exercise. It is the enabling path for broader DOS software compatibility, later audio/device work, and only then Windows pre-NT milestones.
