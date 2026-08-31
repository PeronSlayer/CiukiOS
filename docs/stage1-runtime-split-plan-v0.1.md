# Stage1 Runtime Split Plan v0.1

## Status

**COMPLETED — superseded by the wholesale kernel move.**

Phase 5 is closed. The active `full` and `full-cd` profiles no longer use the incremental resident-front-end architecture described in the historical sections below. They build a dedicated loader-only Stage1 and a wholesale CIUKIDOS kernel containing the normal DOS implementation.

The current loader is 1,542 bytes. The boot sector loads eight sectors for it inside the reserved 72-sector Stage1 slot. It locates, validates, and transfers to `\SYSTEM\CIUKIDOS.SYS`; missing or invalid required components enter a bounded fatal halt. There is no interactive Stage1 fallback and no normal DOS owner in the loader.

## Governing Boundary

Stage1 is the boot loader, not the final operating-system owner.

Stage1 retains only:

1. real-mode entry, stack, and boot-drive initialization
2. the minimum BIOS disk and FAT16 path needed to locate required system files
3. CIUKIDOS load, validation, handoff, and transfer
4. bounded fatal diagnostics when a required component cannot run

CIUKIDOS owns:

1. DOS interrupt dispatch and runtime state
2. PSP, DTA, MCB, and conventional-memory lifecycle
3. child load, execution, termination, return code, and parent restoration
4. JFT/SFT and handle semantics
5. general file/path APIs and FAT runtime policy
6. COM/MZ execution services and extender-facing DOS behavior
7. device, driver, mouse, XMS, and compatibility policy that is not boot-critical

`SHELL.COM` owns the interactive command processor and must use public DOS/BIOS contracts rather than Stage1-private buffers or labels.

## Current Boot Contract

```text
BIOS
  -> boot sector
      -> load 8 sectors from the reserved 72-sector Stage1 slot
          -> 1,542-byte loader-only Stage1
              -> load and validate \SYSTEM\CIUKIDOS.SYS
                  -> transfer to the CIUKIDOS kernel at 0900h
                      -> initialize ABI2 runtime state and normal DOS ownership
                          -> execute \SYSTEM\SHELL.COM
                              -> child programs through DOS EXEC

Any required-component failure or shell return
  -> bounded loader-fatal message
  -> halt
```

The current CIUKIDOS flat image is 43,254 bytes, loads at segment `0x0900`, and is build-bounded to `0xA900` (43,264) bytes. Four EXEC frames occupy `0x1400-0x1457`; Stage2 starts at `0x1480`. These are enforced product layout boundaries, not estimates. The remaining ten-byte margin makes the limit an active build constraint.

## Current CIUKIDOS ABI

- Runtime file: `\SYSTEM\CIUKIDOS.SYS`
- Signature: `CIUKIDOS`
- Service-table magic: `RTSV`
- ABI version: `2`
- Descriptor size: `8` bytes
- Service count: `11`
- Capability mask: `0x003F`
- Declared load segment: `0x0900`
- Maximum image size: `0xA900` bytes (43,264 bytes)

The 26-byte image header declares the signature, ABI, service count, descriptor size, capabilities, exact image size, service-table offset, and load segment. The `RTSV` table repeats the ABI/count/descriptor contract and contains eleven ordered, bounded callable descriptors.

| ID | Current service | Ownership level |
|---:|---|---|
| 1 | Identity/status | Runtime-owned, read-only |
| 2 | Version string | Runtime-owned, read-only |
| 3 | Stage2-ready marker | Runtime-owned, read-only |
| 4 | DOS version result | Kernel-owned provider |
| 5 | Default-drive state pointer | Kernel-owned state |
| 6 | Runtime-state pointer | Kernel-owned state |
| 7 | Prepare child DTA/process state | Kernel-owned process-stack mutation |
| 8 | Restore parent DTA/process state | Kernel-owned process-stack mutation |
| 9 | Kernel capabilities and chain identity | Returns `0x003F` and a deliberately null legacy vector |
| 10 | Synchronize complete process/DTA view | Kernel-owned state synchronization |
| 11 | Record child termination | Kernel-owned, identity-checked termination record |

CIUKIDOS owns the complete normal `INT 20h`/`INT 21h` path. There is no preserved Stage1 DOS vector and no legacy far-chain target. The black-box contract reports this explicitly as `CHAIN=0`.

The ABI contract requires all eleven services. Validation against the historical ABI1 five-, eight-, or ten-service checkpoints is invalid for the current product.

### Black-box ownership probe

The build packages `src/com/ciukrtst.asm` as `\APPS\CIUKRTST.COM`. From a normal external child it verifies:

1. IVT `INT 21h` ownership in kernel segment `0x0900`
2. ABI version 2, eleven descriptors, and service IDs 9, 10, and 11
3. service 9 capability mask `0x003F` and a zero legacy chain target
4. DOS version, default drive, current PSP, and set/get/restore DTA coherence

Its exact success marker is:

    [CIUKRTST] OWNER=CIUKIDOS ABI=2 SERVICES=11 CHAIN=0 STATE=PASS

The full and full-CD shell gates consume this marker. Its presence in source or in an image is not a PASS until the corresponding same-checkout lane records it and returns cleanly to the shell.

### Process, TSR, and MZ hardening already implemented

The wholesale CIUKIDOS kernel and its process record enforce the following general behavior:

1. immutable EXEC identity, independent of application-visible `AH=50h` PSP changes
2. four ordered nested process frames for parent PSP/DTA/allocator restoration
3. `AH=31h` residency for the child PSP plus auxiliary child-owned blocks
4. exact resident-block unload through `AH=49h`, including cross-owner unloader use, double-free rejection with `AX=0009h`, and strict `AH=4Ah` ownership
5. resident-aware, title-independent first-fit COM/MZ placement and low-memory MCB reconstruction, including the 256,000-byte doom-vanille allocation path and DOSNavigator's COM→MZ chain
6. 32-bit MZ size/relocation validation plus a bounded DOS-compatible final-page read for historic COM-to-EXE images whose physical EOF extends beyond strict `e_cblp`

The packaged `CIUKPST.COM`/`CIUKTRM.COM`/`CIUKPCOM.COM` lane installs a real resident child, executes another child while the TSR remains installed, verifies its PSP/MCB/image sentinels, unloads it, walks the linked MCB chain, and requires a second free to fail. Its exact completion marker is:

    [PSTACK:C] ALL=PASS TSR-EXEC-UNLOAD=PASS EXIT=5A

These are kernel compatibility and lifecycle guarantees. The current fixed implementation still has explicit architectural caps of 32 tracked memory blocks and four simultaneous EXEC contexts. Raising those bounds, deepening JFT/SFT semantics, and validating more real applications are Phase 6 compatibility work; they do not reopen the completed Phase 5 ownership boundary.

## Historical Pre-Wholesale Ownership Gap — Superseded

> Historical record: the following gap described the final incremental ABI1/front-end checkpoint. The wholesale ABI2 kernel move closed this ownership gap; these items are retained to explain why the incremental plan was superseded.

At that checkpoint, these normal runtime responsibilities remained primarily in `src/boot/floppy_stage1.asm`:

1. most `INT 21h` API implementations behind the CIUKIDOS far-chain boundary
2. PSP/MCB construction and allocator policy
3. COM/MZ load, relocation, child transfer, and termination
4. general handles, file/path operations, directory mutation, and FAT writes
5. environment and process restoration
6. much of mouse, driver, and compatibility state

The obsolete `src/runtime/runtime.asm` five-service artifact was removed. Dated evidence may still mention `RUNTIME.BIN`, ABI1, segment `0x1100`, or a chained Stage1 core as historical context; none describes the active product.

## Historical Incremental Migration Order — Completed by Wholesale Move

> Historical record: these tranches were the conservative sequence before the decision to compile the complete normal DOS core into CIUKIDOS. They are no longer open Phase 5 work. Compatibility depth mentioned inside them may still motivate Phase 6 issues.

### Tranche 1 - Contract and negative-path hygiene

1. Keep the eleven-service requirement explicit and versioned.
2. Validate missing, truncated, bad-signature, bad-header, incompatible-ABI, and missing-service cases.
3. Require the loader-fatal result and absence of a shell prompt for required-runtime failures.
4. Remove remaining `RUNTIME.BIN`, five-service, 70-sector, and interactive-fallback assumptions.

### Tranche 2 - Process state ownership

1. Build on the existing CIUKIDOS PSP/DTA ownership by moving return code and the remaining child lifecycle state into the runtime.
2. Expose operations rather than writable internal pointers where practical.
3. Prove nested COM and MZ execution plus exact parent restoration.

### Tranche 3 - Conventional memory ownership

1. Move MCB arena initialization, allocation, resize, free, and largest-block reporting.
2. Remove fixed layout assumptions that leave large MZ children with an artificially small arena.
3. Use doom-vanille's 256,000-byte low-memory request as a cross-workload regression target, not as a title-specific patch.

### Tranche 4 - EXEC ownership

1. Move COM/MZ validation, relocation, environment/PSP setup, transfer, and termination.
2. Preserve shell return and nested extender behavior.
3. Keep title-specific binary patching outside the compatibility contract.

### Tranche 5 - Handles, paths, and file APIs

1. Move JFT/SFT, standard handles, open/read/write/seek/close, find, directory, and rename/move behavior.
2. Prove root and subdirectory FAT16 mutations, high-cluster access, error mapping, and stale-state cleanup.
3. Do not accept a PARTIAL file operation as a passing release gate.

### Tranche 6 - Interrupt and device policy

1. Expand the installed CIUKIDOS `INT 21h` front end until Stage1 is no longer the normal DOS service implementation.
2. Move non-boot-critical device, mouse, driver, XMS, and diagnostic policy behind runtime/module contracts.
3. Leave Stage1 with only the minimum boot services needed before CIUKIDOS is live.

## Validation Gates

The completed boundary is protected by both static and dynamic gates on the same checkout:

1. `scripts/verify_phase5_runtime_ownership.sh` proves that `full` binds to `full_stage1_loader.asm` and canonical `ciukidos.asm`; the loader source/listing contains no normal DOS interrupts or owner symbols; the kernel listing contains the required DOS owners; no far chain to a previous `INT 21h` owner exists; and every ABI2 header, table, descriptor, size, load-segment, and handler-bound invariant is valid.
2. `scripts/qemu_test_full_runtime_probe.sh` first builds the exact canonical kernel, runs the external `CIUKRTST.COM` black-box ABI2/owner probe, and verifies the static ownership boundary. It then runs eight independent fatal negatives from a hash-verified good image: missing file, truncated image, bad signature, bad header size, incompatible ABI, missing header service count, missing table service count, and missing required descriptor. Every negative must emit the `WOOF` fatal result, must not reach CIUKRTST, and must not reach a Stage1 or SHELL prompt. The canonical image is restored on every exit path. This avoids a probe-only kernel variant that could exceed the product's ten-byte size margin.
3. `CIUKRTST.COM` supplies the black-box ownership assertion `ABI=2 SERVICES=11 CHAIN=0` from a normal child.

The wider regression set remains:

1. `make build-full`
2. `make build-full-cd`
3. `make qemu-test-full`
4. `make qemu-test-full-cd`
5. `make qemu-test-full-runtime-probe`
6. `make qemu-test-full-shell-com`
7. `make qemu-test-full-shell-com-boot`
8. `make qemu-test-full-cd-shell-com-boot`
9. `make qemu-test-full-dos-compat-smoke`
10. `make qemu-test-setup-runtime-hdd-install` when install/runtime layout is touched
11. `make qemu-test-all`
12. affected DOOM, WOLF3D, driver, and audio taxonomy lanes when shared DOS paths change

A focused PASS does not override an aggregate or negative-path failure. Historical evidence is not a substitute for a fresh run after an ownership change.

Same-checkout closure snapshot (2026-09-01): the loader-only/static ownership gate, exact canonical-kernel ABI2 positive probe, eight-case fatal matrix derived from the same hash-verified image, full/full-CD shell and ownership probes, DOS compatibility smoke, shell stability, DRVLOAD, deterministic full-CD read beyond LBA 65,535, setup packaging, CD/HDD read-only probe, host-built HDD boot, and runtime CD-to-HDD install provide the Phase 5 boundary evidence. Later Costa, DOSNavigator, networking, and Windows 3.1 gates exercise the same wholesale owner without changing the closure definition.

## Definitive Phase 5 Completion Gate

Phase 5 is complete because all of the following ownership conditions are satisfied:

1. Stage1 is limited more strictly than originally required: boot-critical loading, validation, handoff, and fatal diagnostics; CIUKIDOS starts the shell.
2. CIUKIDOS owns normal DOS interrupt, process, memory, handle, file/path, and COM/MZ execution state.
3. The shell and child programs do not depend on Stage1-private DOS state.
4. The ABI and memory layout are documented, versioned, and have positive and negative tests.
5. Obsolete runtime artifacts and fallback assumptions are gone.
6. Full and full-CD runtime, shell, compatibility, installer-impact, and aggregate gates are green from the same checkout.

Phase 6 is **ACTIVE**. Logical JFT/SFT depth, broader handle semantics, and the quantitative external-program corpus are Phase 6 compatibility closure items. They are not missing Phase 5 ownership tranches: the corresponding implementation now lives inside CIUKIDOS, and Phase 5 concerns the code/state boundary rather than proving complete compatibility with arbitrary DOS software.
