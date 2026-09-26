# Post-Runtime Compatibility Roadmap v0.1

## Current increment — 2026-09-26

The native desktop increment has eleven passing, narrowly scoped QEMU reports;
see [current milestones](current-milestones.md) and
[the validation archive](validation/2026-09-26/README.md). The selected image
contains a 43,169-byte kernel. Existing BIOS-text windows and cooperative game
ports do not implement arbitrary original DOS graphics applications in windows.

The next implementation is an opt-in V86 session with virtual VGA memory/ports,
protected-mode presentation and coordinated DPMI/peripheral ownership. Its
[architecture record](dos-window-architecture-2026-09-26.md) separates reusable
Jemm/HDPMI components from missing CiukiOS integration. Existing fullscreen and
boot paths remain the compatibility baseline. Physical T23/E500 results and
the broader external DOS corpus remain separate acceptance requirements.

## Historical phase baseline — September 1

1. Phase 5 runtime ownership: **COMPLETED by the wholesale CIUKIDOS kernel move**.
2. Current kernel contract: **CIUKIDOS at `0x0900`, `ABI=2`, 11 services, 8-byte descriptors, capability mask `0x003F`, `CHAIN=0`; 43,167-byte `full` and 43,162-byte D:-default `full-cd` artifacts with maximum `0xA900` (43,264) bytes**.
3. Current loader/layout contract: **1,542-byte loader-only Stage1; eight sectors loaded inside the reserved 72-sector slot; four EXEC frames at `0x1400-0x1457`; Stage2 at `0x1480`**.
4. Phase 6 DOS application compatibility: **ACTIVE and not closed because the quantitative external corpus and several logical compatibility surfaces remain incomplete**.
5. Phase 7 legacy audio: **controlled groundwork available, but not closed; formal closure work follows broader Phase 6 evidence**.
6. Phase 8 networking: **ACTIVE with the first bounded Packet Driver/IPv4/FTP milestone complete**.
7. Phase 9 Windows pre-NT: **ACTIVE with a bounded Windows 3.1 Enhanced Mode milestone complete; the phase remains open**.

Phase 5 closure establishes where the normal DOS implementation and state live; it does not assert perfect DOS semantics. Logical JFT/SFT depth, broader handle behavior, and the required external-program corpus belong to Phase 6 and do not reopen Phase 5. A compatibility workaround must not replace a general kernel fix.

## Phase A - Runtime Ownership — Completed

Goal achieved: CIUKIDOS, rather than Stage1, is the normal DOS runtime owner.

Closure evidence:

1. the active `full` build binds a loader-only Stage1 and a canonical wholesale CIUKIDOS kernel
2. the loader contains no normal `INT 20h`/`INT 21h` owner or DOS implementation symbols, while the CIUKIDOS listing contains them
3. the ABI2 image and all eleven descriptors are size- and target-bounded, and service 9 reports no previous DOS chain target
4. `CIUKRTST.COM` reports `OWNER=CIUKIDOS ABI=2 SERVICES=11 CHAIN=0 STATE=PASS`
5. process, MCB, COM/MZ EXEC, termination, restoration, file/path, handle, FAT, interrupt, and device implementations are compiled into CIUKIDOS rather than Stage1
6. `scripts/verify_phase5_runtime_ownership.sh` statically rejects a legacy chain or ownership regression
7. `scripts/qemu_test_full_runtime_probe.sh` requires the positive ABI path and eight fatal negatives: missing, truncated, bad signature, bad header size, incompatible ABI, missing header count, missing table count, and missing descriptor; each negative requires `WOOF` and absence of a prompt

Satisfied exit signal:

Stage1 is limited to boot-critical loading, CIUKIDOS validation/handoff, and fatal diagnostics. CIUKIDOS owns normal DOS execution and shell startup; the full/full-CD ownership, positive, and fatal-negative gates protect that boundary.

The completed ownership contract and the clearly marked historical incremental plan are in `stage1-runtime-split-plan-v0.1.md`.

## Phase B - Broaden DOS Application Compatibility

**Status: ACTIVE.**

Goal: host a mixed, repeatable set of unmodified external DOS software.

Current known boundaries:

1. GPL CuteMouse has a repeatable `full` install/INT33/unload/restore PASS, but no equivalent full-CD workflow is recorded.
2. DOSNavigator is now packaged byte-for-byte from upstream and validates dual-pane startup, generic allocation, one-row keyboard navigation, mouse input, Colors/XMS, native `Alt+X` exit, shell recovery, and same-boot PSP/mouse/video cleanup. It remains PARTIAL because broad file/editor and full-CD workflows are missing.
3. WOLF3D reaches `visual_gameplay`, keyboard-driven menus, and a correctly rendered first level with the injected page-flip/VGA Attribute Controller workaround; it remains PARTIAL until unmodified-binary, clean-exit, full-CD, and audio workflows are proven.
4. doom-vanille passes the focused 256 KiB low-DOS allocation gate, real gameplay/HUD/wall-integrity, SB device 3/code 8 selection, DMX initialization, and objective non-silent PCM after restoring its required Watcom ABI and one-byte structure packing. Clean completion and full-CD execution remain unproven.
5. arbitrary application evidence from the full-CD D: environment is missing.
6. internal CIUKRTST/PSTACK probes are stronger than the external application corpus and do not count toward it.
7. JFT/SFT structures exist under CIUKIDOS ownership, but DOS-compatible sharing, inheritance, duplication, close-on-exec, device, and error behavior still need workload-driven logical validation. This is Phase 6 compatibility work, not unfinished Phase 5 placement.

Exit signal:

1. at least ten external applications on `full`, with two in each required category
2. at least five redistributable representatives spanning all categories on `full-cd`
3. at least 80% PASS, with every non-PASS result classified
4. no critical memory, storage, handle, process-restoration, or shell-return defect
5. no PARTIAL or title-specific patch counted as general PASS

The authoritative workload table is in `dos-compatibility-matrix-v0.1.md`.

## Phase C - Legacy Audio

Goal: move from controlled device evidence to game-level external audio compatibility.

Already proven in controlled lanes:

1. SB16 DSP detection and initialization
2. real-mode DMA1/IRQ7 playback
3. protected-mode timer and SB IRQ delivery
4. DOOMSFX playback of selected WAD lumps
5. external AdLib/OPL2 music with objective waveform evidence
6. original Doom real gameplay with an objectively non-silent OPL2-music/PC-speaker-SFX WAV
7. doom-vanille simultaneous OPL2 music plus SB16 SFX with DMX return 10

Still open:

1. original DOOM SB16/DMX protected-mode invalid-opcode failure
2. doom-vanille audio-enabled clean completion and shell recovery after its now-passing combined OPL2/SB16 gameplay output
3. another external DOS audio workload with clean completion
4. a second external AdLib/OPL or SB workload with clean completion
5. full-CD and real-hardware audio evidence

Exit signal:

DOOM reaches a documented repeatable audio target and at least one other external DOS audio program completes detection, initialization, practical playback, and clean return.

## Phase D - Legacy Networking

**Status: ACTIVE; first bounded milestone complete.**

The `full` profile now has a pinned open-source Crynwr NE2000 Packet Driver and mTCP IPv4/FTP lane. `NETSTART` adds a resident Packet Driver bridge so ARP/ICMP remains available independently of `FTPSRV`; `NETCFG` persists IP, mask, gateway, and DNS. QEMU user networking provides outbound Internet access and forwarded FTP, while Linux TAP/NAT makes the guest host-routable. Disposable gates validate Internet Echo, FTP transfer/persistence, and a checksum-valid inbound Echo with no FTP server running.

This explicit Phase 8 advance does not close Phase 6 or Phase 7 and does not imply broad network support. DHCP product evidence, other NICs, physical/bridged networking beyond the isolated TAP lane, encrypted transfer, SMB/CIFS, WebDAV, SFTP, IPv6, and full-CD remain open.

## Phase E - Windows pre-NT

**Status: ACTIVE; first bounded Windows 3.1 milestone complete.**

Current evidence:

1. Optional user-supplied Windows 3.1 media is integrated into the canonical `full` image without tracking or redistributing it.
2. Windows reaches 386 Enhanced Mode through the normal `WIN` command using general DOSMGR/SDA, XMS/A20, PS/2 BIOS mouse, device-chain, EXEC-owner, and handle-restoration interfaces.
3. The focused gate proves two 640x480 Program Manager starts, exactly one linear 1:1 pointer, native Sound Blaster startup WAV audio with AdLib MIDI configured, Calculator launch, task-scoped `Alt+F4`, Program Manager survival, clean CiukiOS shell recovery, and relaunch.

Still open: wider Windows 3.1 application/device/multimedia/printing coverage, full-CD, real hardware, Windows 95, and Windows 98. The early result does not satisfy or waive the still-open Phase 6 and Phase 7 exit gates.

## Non-Priority Work

1. FAT32 is future scope, not a prerequisite for Phase 5-7.
2. The GUI demo is exploratory and does not alter the critical path.
3. Floppy/FAT12 remains a legacy/minimal lane unless explicitly reopened.
4. Multi-floppy installer media and external-storage automount remain separate planned work, not hidden prerequisites for Phase 5 or Phase 6 activation.
