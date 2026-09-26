# CiukiOS Legacy x86 Architecture v1

## 1. Architectural Objective
Define a native legacy x86 (BIOS) architecture, without UEFI dependency in the new core and without CPU emulation as the final execution model for DOS/pre-NT workloads.

## 2. Non-negotiable Principles
1. Boot from real legacy BIOS hardware.
2. Execute DOS workloads natively on real CPU behavior.
3. Advance compatibility through measurable milestones.
4. Maintain the legacy/minimal `floppy` profile and the active `full` and `full-cd` product profiles.
5. Keep active engineering priority on the `full` and `full-cd` runtime paths unless the owner explicitly reopens another profile.

## 3. System Layers
1. Stage-B0 (Boot Sector): minimal 16-bit boot entry (512B) that chains to next stage.
2. Stage-B1 (Extended Loader): memory setup, boot-media access, kernel loading.
3. Stage-K (Kernel Core): interrupt routing, memory manager, simple scheduler/event loop.
4. Stage-D (Native DOS Runtime): COM/EXE loader, PSP/MCB, `INT 21h/10h/13h/16h/1Ah/33h`.
5. Stage-G (Graphics/Desktop): VGA/VBE and native compatibility surface for desktop runtime paths.
6. Stage-A (Applications): DOS tools, broad DOS compatibility targets, DOOM/audio milestones, bounded networking, and progressive Windows pre-NT milestones.

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
1. Target: extended disk image (`FAT16` today, `FAT32` later if explicitly prioritized) for full runtime.
2. Content: graphics stack, desktop runtime path, advanced DOS tools, complex app targets.
3. Purpose: complete operating environment.

### 5.3 `full-cd` profile
1. Target: BIOS-bootable live/install media containing the FAT16 runtime environment.
2. Content: external shell on D:, installer payload, and the same required CIUKIDOS runtime boundary as `full`.
3. Purpose: live operation, installation, and profile-specific compatibility validation.

## 6. Canonical Phase 5 Ownership Boundary

1. Phase 5 is closed for both `full` and `full-cd`: Stage1 is a 1,542-byte loader-only component at `0x0800`, and stage0 loads 8 sectors while retaining the 72-sector reserved BPB/disk slot.
2. The wholesale `CIUKIDOS.SYS` DOS kernel is resident at `0x0900`. Its stable image/service contract is `ABI=2`, 11 descriptors of 8 bytes, capability mask `0x003F`, and `CHAIN=0`; Stage1 is no longer a fallback DOS owner.
3. The current kernel artifacts are 43,167 bytes on `full` and 43,162 bytes on the D:-default `full-cd` profile; the enforced maximum is `0xA900` (43,264 bytes). Four external EXEC-state frames occupy `0x1400-0x1457`; Stage2 starts at `0x1480`.
4. The closure bundle passes static ownership and eight negative ABI cases, normal and high-LBA `full`/`full-cd` boot, nested COM/MZ and TSR restoration, direct-CD-to-HDD installation, CuteMouse, focused doom-vanille and WOLF lanes, DRVLOAD smoke, and repeated shell stability.
5. This boundary does not close Phase 6. Full JFT/SFT depth and the required external-application corpus remain explicit backlog.

## 7. Compatibility Targets
1. DOS applications: highest priority baseline.
2. Runtime split: keep Stage1 loader-first and move runtime ownership into loaded components.
3. DOOM: graphics, execution, and later audio compatibility milestone.
4. Legacy audio and bounded networking: follow-up compatibility milestones after broader DOS application bring-up.
5. Windows pre-NT (up to 98): the bounded Windows 3.1 Enhanced Mode milestone is proven on `full`; broader Windows 3.1 plus Windows 95/98 remain progressive follow-ups.

## 8. Quality Requirements
1. Deterministic serial logging for boot and critical interrupt paths.
2. Automated tests per milestone.
3. Validation on both emulators and real legacy hardware.

## 9. Explicit Exclusions
1. UEFI dependency in the new runtime core.
2. CPU software emulation as final architecture.
3. Windows NT and newer scope.
