# CiukiOS Legacy v2 Roadmap

## Vision
Build a simple, native x86 BIOS operating system that runs DOS and pre-NT workloads without CPU emulation in the final runtime path.

## Normative References
1. DOS core contract: `docs/dos-core-spec-v0.1.md`
2. DOS core execution plan: `docs/dos-core-implementation-plan-v0.1.md`
3. Phase 5 ownership closure record: `docs/stage1-runtime-split-plan-v0.1.md`
4. Phase 6 workload and exit matrix: `docs/dos-compatibility-matrix-v0.1.md`
5. Phase 7 evidence boundary: `docs/legacy-audio-bring-up-plan-v0.1.md`
6. Current cross-phase milestone ledger: `docs/current-milestones.md`

## Phase 0 - Reset Foundation
1. Reset the previous project state and establish a clean legacy-first baseline.
2. Define and freeze legacy-first architecture.
3. Establish new AI and development operating rules.

## Phase 1 - Minimal Legacy Boot (Floppy-first)
1. 16-bit boot sector (512B) and multi-stage loader.
2. Real-mode initialization and core BIOS services (`INT 10h/13h/16h/1Ah`).
3. Minimal x86 kernel and basic shell.
4. `floppy` profile constrained to 1.44MB.

## Phase 2 - Native DOS Runtime
1. Native `.COM/.EXE` loader.
2. PSP/MCB and conventional memory management (with future UMB/HMA extensions).
3. High-compatibility `INT 21h` surface.
4. FAT12/FAT16 baseline for floppy profile.

## Phase 3 - DOS Graphics Runtime (Shell-first)
1. Native VGA/VBE path.
2. Extended `INT 10h` plus robust timer/mouse/input services.
3. Incremental graphics services that keep shell stability as primary target.
4. Milestone: stable graphics/runtime services on real hardware with shell loop preserved.
**STATUS: CLOSED (2026-04-30)**
- Evidence: released `CiukiOS pre-Alpha v0.5.3` after shell `move/mv` runtime stabilization.
- Evidence: fixed `INT 21h AH=56h` rename/move behavior with deterministic semantics.
- Evidence: reran floppy (FAT12) and full (FAT16) regression lanes with stable outcomes.

## Phase 3.5 - CiukiOS Installer (Setup project)
> Tracked separately under `setup/`. Prerequisite: stable Phase 3 runtime.
1. DOS-Setup-style text-mode TUI installer binary (`SETUP.COM`).
2. Multi-floppy distribution: N × 1.44MB images with disk-swap engine.
3. CD-ROM distribution: single bootable ISO 9660 image.
4. Installation flow: drive detection, FAT16 format, file copy, config write.
5. Component selection: Minimal / Standard / Full.
**STATUS: CLOSED - FUNCTIONAL MVP (FULL-only) (update 2026-05-01)**
- Historical trace: Phase 3.5 was first closed as a FOUNDATION/PLACEHOLDER baseline on 2026-04-30.
- Functional closure update (2026-05-01): MVP installer baseline is executable on the full profile, with `SETUP.COM` packaged in the FAT16 full image.
- Evidence (2026-05-01): `scripts/qemu_test_full_stage1.sh` PASS.
- Evidence (2026-05-01): `scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Scope caveat: closure is full-profile only; floppy lane is not a required installer baseline.
- Advanced backlog note: multi-floppy distribution and extended CD installer workflow remain post-MVP backlog items for a later phase.
- See `setup/README.md` for post-MVP installer maintenance and advanced backlog tracking.

## Phase 4 - DOOM Milestone + Installer Execution Track
**STATUS: CLOSED - DOOM GAMEPLAY PLAYABLE (2026-05-04)**
**INSTALLER EXECUTION LANE: CLOSED (2026-05-03)**
**RUNTIME/DOOM LANE: CLOSED - PLAYABLE (2026-05-04)**
1. Installer execution backlog (post-MVP hardening, media-swap flows, and failure-path validation) is completed.
2. Reproducible installer evidence bundle is completed and archived.
3. Mode 13h/VGA and DOS extender compatibility reached the level required for DOOM gameplay on the full FAT16 profile.
4. Minimum extender compatibility for complex DOS binaries is proven through DOS/4GW startup, WAD loading, refresh/playloop/input/sound/HUD/status-bar initialization, and rendered gameplay.
5. Milestone: DOOM boots and is playable. **COMPLETED (2026-05-04)**
- Evidence (2026-05-01): released `CiukiOS pre-Alpha v0.5.4` after shell input stability improvements (hold-key repeat, wrap, backspace) and FAT16 footer telemetry stabilization (`CPU/DSK/RAM`).
- Evidence (2026-05-01): reran cross-profile build/regression lanes for floppy (FAT12) and full (FAT16) with stable high-level outcomes.
- Evidence (2026-05-03): `./scripts/build_full.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_full_acceptance.sh` PASS.
- Evidence (2026-05-03): `./scripts/qemu_test_setup_installer_scenarios.sh` PASS.
- Evidence (2026-05-04): `make build-full` PASS.
- Evidence (2026-05-04): `make qemu-test-full` PASS.
- Evidence (2026-05-04): `DO_BUILD=0 DOOM_TAXONOMY_DISPLAY_MODE=none DOOM_TAXONOMY_SCREENSHOT=build/full/doom_post_status_after_rebuild.ppm QEMU_TIMEOUT_SEC=120 DOOM_TAXONOMY_OBSERVE_SEC=45 DOOM_TAXONOMY_MIN_STAGE=video_init make qemu-test-full-doom-taxonomy` PASS.
- Evidence (2026-05-04): visual screenshot `build/full/doom_post_status_after_rebuild.png` shows DOOM gameplay viewport and HUD after status-bar initialization.
- Evidence (2026-05-04): project owner manually confirmed DOOM is playable interactively on the generated full-profile image.
- Release: `CiukiOS pre-Alpha v0.6.1`.
- Scope note: follow-up audio/driver polish and richer gameplay taxonomy remain hardening work, not Phase 4 closure blockers.

## Phase 5 - Runtime Ownership Transition
**STATUS: COMPLETED (2026-08-28); NORMAL DOS RUNTIME OWNERSHIP CLOSED**
1. The active full and full-CD profiles use a 1,542-byte loader-only Stage1. Stage0 loads 8 sectors while the BPB-reserved Stage1 slot remains 72 sectors for the on-disk layout and installer contract.
2. Stage1 locates and validates `\SYSTEM\CIUKIDOS.SYS` and loads the normal DOS kernel at segment `0x0900`. It contains no normal DOS interrupt, process, allocator/MCB, handle, file/path, COM/MZ, or device ownership and exposes no interactive compatibility fallback.
3. The versioned kernel contract is `ABI=2`, 11 descriptors of 8 bytes, capability mask `0x003F`, and a closed Stage1 chain. The current artifact is 43,254 bytes with a hard maximum of `0xA900` (43,264 bytes); EXEC snapshot frames occupy `0x1400-0x1457`, and Stage2 starts at `0x1480`.
4. CIUKIDOS owns the live normal DOS path, launches and restores `\SYSTEM\SHELL.COM` and child processes, and fails closed when the required kernel or shell is missing, invalid, or unexpectedly returns. The black-box ownership marker records `[CIUKRTST] OWNER=CIUKIDOS ABI=2 SERVICES=11 CHAIN=0 STATE=PASS`.
5. The dedicated ownership-boundary check plus current full/full-CD positive, negative, shell, process/TSR, installer-impact, and aggregate evidence satisfy the Phase 5 completion gate on the same checkout.
6. Physical runtime ownership is closed. Broader logical DOS compatibility, including stronger JFT/SFT and handle semantics, remains Phase 6 work and does not reopen Phase 5.

## Phase 6 - DOS Application Compatibility
**STATUS: ACTIVE; NOT A PREREQUISITE FOR THE CLOSED PHASE 5 OWNERSHIP MILESTONE**
1. Make CiukiOS capable of launching a broader set of arbitrary real DOS programs from the full and full-CD profiles.
2. Build a compatibility matrix across utilities, editors, file managers, real-mode games, DOS extender applications, and setup tools.
3. Classify failures by subsystem so fixes broaden compatibility across multiple workloads, not only milestone demos. Logical JFT/SFT, handle, process, and file semantics remain compatibility work here even though their normal runtime code is physically CIUKIDOS-owned.
4. Current evidence is mixed: the GPL CuteMouse install/INT33/unload/restore workflow passes on `full`; official Costa v1.8.0 passes a pinned-download, package, launch, one-cursor, Calculator, and return workflow; doom-vanille passes the 256 KiB low-DOS gate but a direct gameplay capture exposes corrupted planar rendering; the packaged WOLF3D copy reaches keyboard-driven menus and correctly rendered first-level gameplay but remains PARTIAL for unmodified-binary, clean-exit, full-CD, and audio evidence; DOSNavigator passes mouse, one-row navigation, Colors/XMS, `EXIT`, and shell return but remains PARTIAL because of its bounded packaged-loader compatibility hook and missing broad file/full-CD coverage; and the required external corpus plus five-category full-CD matrix are still missing.
5. Exit gate: a documented corpus of at least ten external programs, with at least two programs in each category above, has meaningful workflow and clean-return results on `full`; at least five redistributable representatives spanning all categories also run from `full-cd`.
6. Exit gate: at least 80% of the required matrix cases are PASS, every non-PASS case has a subsystem classification, no PARTIAL result is accepted as PASS, and no open critical defect corrupts memory, storage, handles, or shell return state.

## Phase 7 - Legacy Audio Compatibility
**STATUS: GROUNDWORK AVAILABLE; NOT CLOSED; FORMAL PHASE QUEUED UNTIL PHASE 6 IS BROADER**
1. Close the current game-level audio compatibility gap for DOOM and similar DOS workloads.
2. Investigate and implement the minimum sound-device compatibility required for conservative Sound Blaster and AdLib bring-up.
3. Validate detection, initialization, and practical playback separately.
4. Current groundwork: SB16 detection/init, real-mode DMA/IRQ, protected-mode IRQ/timer probes, and controlled DOOMSFX WAD playback have repeatable evidence. The DOS environment MCB, `BLASTER` propagation, and conventional-memory starvation that blocked doom-vanille are fixed generally; doom-vanille now selects SB device 3/code 8 and completes DMX initialization.
5. Open boundary: the final doom-vanille gameplay WAV is digital silence despite the active SB16 stream, and the proprietary DOOM SB16/DMX path remains unsupported. WOLF3D video and initial keyboard interaction are now validated, but its game-level audio still has no playback evidence.
6. Exit gate: DOOM reaches a documented, repeatable game-level audio target and at least one additional external DOS audio workload completes detection, initialization, playback, and clean return.

## Phase 8 - Legacy Networking
**STATUS: ACTIVE; FIRST BOUNDED MILESTONE COMPLETE**
1. The `full` profile now packages pinned GPL mTCP/Crynwr payloads, sources, and licenses and exports `MTCPCFG=C:\NET\MTCP.CFG`.
2. QEMU exposes NE2000 at IRQ 3 / I/O `300h`; Crynwr installs its Packet Driver at `INT 60h`; mTCP uses static IPv4 `10.0.2.15/24` behind QEMU user NAT.
3. FTP provides bidirectional Linux/Windows host exchange through the sandboxed `C:\SHARE`, with localhost control forwarding on port 8021 and passive ports 2048-2303.
4. `make qemu-test-full-network-ftp` proves Packet Driver initialization, FTP login/list, byte-identical download/upload, FAT16 persistence, and canonical-image immutability.
5. Open boundaries: DHCP product evidence, additional Packet Driver/NIC targets, bridged/physical networking, DNS workflows, encrypted transfer, SMB/CIFS, WebDAV, SFTP, IPv6, and a full-CD lane.
6. FTP is plaintext; the default account and forwarding remain restricted to localhost QEMU NAT or a trusted isolated LAN.

## Phase 9 - Windows pre-NT Milestones
**STATUS: ACTIVE; FIRST WINDOWS 3.1 BOUNDED MILESTONE COMPLETE (2026-09-01)**
1. The canonical `full` image optionally packages user-supplied Windows 3.1 media and an installed tree; none of that proprietary payload is tracked or redistributed.
2. Windows 3.1 reaches 386 Enhanced Mode through `WIN` with DOSMGR/SDA, XMS/A20, PS/2 BIOS mouse, DOS device-chain, EXEC-owner, file-handle, and exit-state compatibility supplied through general interfaces rather than executable-name rules.
3. The focused lane proves two 640x480 Enhanced Mode starts, exactly one moving pointer, Calculator launch, task-scoped `Alt+F4`, Program Manager survival, clean CiukiOS shell restoration, and relaunch.
4. Open boundaries: Windows 3.1 full-CD, broader application/device/audio/printing coverage, real-hardware evidence, Windows 95, and Windows 98.
5. This early bounded result does not waive the Phase 6/7 exit gates or close Phase 9 as a whole.

## Phase 10 - Build and Release Discipline
**STATUS: ACTIVE CROSS-CUTTING WORK; NOT CLOSED**
1. `floppy` profile: minimal, portable, diagnostics-first.
2. `full` profile: complete runtime with shell-first behavior.
3. `full-cd` profile: live/install behavior with D: drive semantics and installer safety gates.
4. Regression pipeline on emulators and real legacy hardware.
- Evidence (2026-09-01): focused aggregate PASS; deterministic full-CD read beyond LBA 65,535 PASS; shell stability and DRVLOAD PASS; runtime direct-CD-to-HDD install and autonomous C: boot PASS; focused Costa, DOSNavigator, networking, and Windows 3.1 compatibility lanes PASS within their documented boundaries.
- Boundary: Phase 5 ownership closure is enforced independently by its loader/kernel boundary gate. The aggregate does not include the long installer or game/audio taxonomy lanes and does not close the Phase 6 external corpus.

## Advancement Criteria
1. Every milestone must have reproducible tests with fresh results from the same checkout as the claim.
2. Aggregate smoke, affected focused lanes, and required negative-path tests must all be green; a narrower focused PASS cannot hide an aggregate failure.
3. Compatibility claims must state profile, workload, minimum stage, observation scope, and whether the payload is internal or external.
4. No merge to `main` without explicit user approval.
5. No CPU-emulation shortcuts as final runtime solution.
