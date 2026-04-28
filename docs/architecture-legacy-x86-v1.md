# CiukiOS Legacy x86 Architecture v1

## 1. Architectural Objective
Define a native legacy x86 (BIOS) architecture, without UEFI dependency in the new core and without CPU emulation as the final execution model for DOS/pre-NT workloads.

## 2. Non-negotiable Principles
1. Boot from real legacy BIOS hardware.
2. Execute DOS workloads natively on real CPU behavior.
3. Advance compatibility through measurable milestones.
4. Maintain two product profiles: `floppy` and `full`.

## 3. System Layers
1. Stage-B0 (Boot Sector): minimal 16-bit boot entry (512B) that chains to next stage.
2. Stage-B1 (Extended Loader): memory setup, boot-media access, kernel loading.
3. Stage-K (Kernel Core): interrupt routing, memory manager, simple scheduler/event loop.
4. Stage-D (Native DOS Runtime): COM/EXE loader, PSP/MCB, `INT 21h/10h/13h/16h/1Ah/33h`.
5. Stage-G (Graphics/Desktop): VGA/VBE and native compatibility surface for desktop runtime paths.
6. Stage-A (Applications): DOS tools, DOOM target, and Windows pre-NT milestones.

## 4. Execution Model
1. Bootstrap in real mode.
2. Controlled transition to protected mode where required.
3. Use hardware-native mechanisms only (no interpreter/JIT CPU emulation in final runtime).
4. Expose DOS-compatible services through interrupt contracts.

## 5. Build Profiles
### 5.1 `floppy` profile
1. Target: 1.44MB BIOS-bootable image (`FAT12`).
2. Content: minimal kernel, shell, diagnostics, and core DOS API subset.
3. Purpose: hardware bring-up, early debugging, recovery path.

### 5.2 `full` profile
1. Target: extended disk image (`FAT16/FAT32`) for full runtime.
2. Content: graphics stack, desktop runtime path, advanced DOS tools, complex app targets.
3. Purpose: complete operating environment.

## 6. Compatibility Targets
1. DOS applications: highest priority baseline.
2. Desktop runtime: primary desktop milestone.
3. DOOM: performance and compatibility milestone.
4. Windows pre-NT (up to 98): progressive milestones, not monolithic delivery.

## 7. Quality Requirements
1. Deterministic serial logging for boot and critical interrupt paths.
2. Automated tests per milestone.
3. Validation on both emulators and real legacy hardware.

## 8. Explicit Exclusions
1. UEFI dependency in the new runtime core.
2. CPU software emulation as final architecture.
3. Windows NT and newer scope.
