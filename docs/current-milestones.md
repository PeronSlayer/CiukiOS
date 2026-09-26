# Current Milestones

Updated 2026-09-26 for `CiukiOS pre-Alpha v0.7.1`. The current desktop increment
and the older cross-phase qualification records have separate evidence scopes.
Dated investigation notes and files under `handoff/` remain historical evidence;
when they disagree with this ledger, `Roadmap.md` and the current compatibility
matrix govern.

## Selected desktop increment

Image: `build/full/native-desktop-2026-09-26/final/ciukios-native-desktop.img`.
SHA-256: `08bc6df6510e55e3f501d49f9414e2d0f0a2006eccdda5dbf8f78bdc1e244bd6`.
The image is a personal 128 MiB development disk, not a public redistribution
or a newly burned ISO. Eleven focused QEMU Pentium III/128 MiB reports pass;
[archived reports and manifests](validation/2026-09-26/validation.json) preserve
their scope. [The implementation record](native-desktop-2026-09-26.md) describes
the source/image identity and failure evidence.

| Area | September 26 result | Explicit limit |
| --- | --- | --- |
| Native Files/media | CD, floppy and BIOS-exposed disk navigation, text preview, import and graphical errors pass. | Removable sources are read-only; USB needs BIOS exposure before boot, with no native hot-plug stack. |
| Wallpaper | 99 tiles, conversion/import/Refresh, persistent selection, damaged-file protection and pixel checks pass. | Indexed tiles up to 256×256; full-size Fit/Fill is not implemented. Windows-origin wallpaper assets remain personal-build-only. |
| System sounds | Kenney CC0 events, ICH AC97/SB PCM, mute, missing-device/file and DOS ownership handoff pass. | Supported controllers only; QEMU playback does not qualify T23/E500 hardware. |
| Windowed DOS | Restricted foreground BIOS-text execution; separately, cooperative Doom preview Ctrl/Space/chords/releases pass. | The games are source ports. Original DOS graphics/DPMI/audio virtualization remains unimplemented. |
| Hardware | No new physical qualification. | T23/E500 input/audio, native hardware acceleration and physical frame rate remain open. |

## Phase Status

The following phase table retains the bounded September 1 application matrix;
it is not a claim that every workflow was rerun on the selected desktop image.

| Phase | Status | Reached milestone | Still required |
|---|---|---|---|
| 3 — DOS graphics/runtime | CLOSED | Stable shell-first graphics/runtime baseline. | Regression maintenance only. |
| 3.5 — Setup | CLOSED for functional `full` MVP | Packaged `SETUP.COM` and accepted installer flow. | Per-row B6 keyboard evidence and post-MVP media work remain backlog. |
| 4 — DOOM gameplay | CLOSED for its accepted 2026-05-04 scope | Original DOOM reached the recorded playable visual milestone on `full`; the current aggregate also verifies a healthy gameplay frame. | SB16/DMX remains outside the Phase 4 closure. |
| 5 — Runtime ownership | CLOSED | Loader-only Stage1 plus wholesale ABI2 CIUKIDOS owner on `full` and `full-cd`. | No placement tranche remains; behavioral compatibility continues in Phase 6. |
| 6 — DOS applications | ACTIVE | Bounded Costa and CuteMouse PASS workflows; stronger DOSNavigator, WOLF3D, doom-vanille, shell, process, editor, mouse, and driver evidence. | Ten-program `full` corpus, five-category `full-cd` corpus, complete JFT/SFT behavior, and removal/productization of title-specific hooks. |
| 7 — Legacy audio | NOT CLOSED | Objective OPL2, real-mode and DOS/4GW SB16 WAV gates, controlled WAD playback, original Doom OPL2/PC-speaker gameplay, and doom-vanille combined OPL2/SB16 gameplay pass. | Original Doom SB16/DMX, clean return from another external workload, full-CD, and real-hardware evidence. |
| 8 — Networking | ACTIVE; first bounded milestone complete | Configurable IPv4, resident ARP/ICMP independent of FTP, outbound Internet ping, host ping over TAP, and bidirectional FTP on `full`. | DHCP-server evidence, more NICs, encrypted/common protocols beyond FTP, full-CD, and physical hardware. |
| 9 — Windows pre-NT | ACTIVE; expanded bounded milestone complete | Optional local Windows 3.1 reaches 386 Enhanced Mode twice with one linear pointer, native SB startup WAV audio, real AdLib MIDI playback, generic DOS-VM enter/exit, unprofiled DOS/4GW Doom with automatic EMS/XMS, task `Alt+F4`, clean shell return, and relaunch. | Wider Windows 3.1 multimedia/application coverage, full-CD, printing/devices, real hardware, Windows 95, and Windows 98. |
| 10 — Build/release | ACTIVE | Canonical full/full-CD build, focused aggregate, negative ABI, installer, application, networking, and Windows gates. | Broader CI/release automation and real-hardware matrix. |

## Canonical Runtime Boundary

- Stage0 loads 8 sectors from the 72-sector reserved Stage1 slot.
- Loader-only Stage1: 1,542 bytes at segment `0x0800`.
- `CIUKIDOS.SYS`: 43,169 bytes in the September 26 selected image at segment `0x0900`; hard ceiling `0xA900` (43,264 bytes).
- ABI: version 2, 11 descriptors, 8 bytes per descriptor, capability mask `0x003F`, `CHAIN=0`.
- Five EXEC-state frames occupy `0x0E00-0x0E6D`; Stage2 starts at `0x0E80`.
- The selected kernel has 95 bytes below its unchanged build-gated ceiling. New monitor/device code must use explicit module ownership rather than silently expanding the validated layout.

## Current Compatibility Decisions

1. Mouse support is one shared DOS/BIOS path, not an executable-name policy. Costa and DOSNavigator use the resident `INT 33h` service; Windows 3.1 uses the IBM PS/2 `INT 15h/AH=C2h` callback path. BIOS PS/2 clients exclusively own their cursor rendering, preventing the former double pointer.
2. COM/MZ placement is title-independent first-fit allocation. Doom-vanille's 256 KiB request and DOSNavigator's COM-to-MZ chain are regression workloads, not kernel name checks.
3. A COM process that launches another program must release unused paragraphs with DOS `INT 21h/AH=4Ah` before nested `AH=4Bh`. `SHELL`, `NETSTART`, `DRVLOAD`, `PMIRQSB`, `DOOMSFX`, `DOOMVAN`, `DOOMSB`, and `NETCFG` relocate their live stack and follow this standard contract; the kernel contains no program-name exception.
4. Canonical interactive QEMU runs use SDL through a verified X11/XWayland transport, explicit i8042, 256 MiB VM RAM, approximately 63 MiB XMS-visible memory, one Pentium III vCPU, and KVM for the widest stable application set. The opt-in `vga-fast` TCG JIT profile is measurably faster for verified-safe planar-VGA workloads, but QEMU 11.1 TCG crashes during original Doom gameplay; `auto`, `tcg`, and `tcg-safe` remain explicit diagnostic choices.
5. Windows 3.1 media, Doom, WOLF3D, DOSNavigator, and other non-redistributable payloads remain local and ignored. Their tests must SKIP or report missing payload honestly when absent.
6. A historical or narrow PASS never overrides a newer direct result. Doom-vanille's allocation, texture-integrity, DMX/SB16 selection, and non-silent PCM gates are green on the current checkout.
7. The external shell owns its display after a normal child returns. It restores BIOS mode 03h, font/page/cursor/intensity state, title bar, and prompt generically; applications are not required to know CiukiOS shell internals.
8. Windows DOS profiles request all available conventional/EMS/XMS memory instead of fixed per-program limits. DOSMGR-selected synthetic PSPs receive an automatic conventional arena, and each Win16/DOS-VM EXEC frame above the Windows startup baseline is released independently on termination; no Doom-specific PIF or executable-name exception is used.

## Validated Bounded Workflows

These are the historical application results summarized on September 1. The
newer desktop report does not convert their open limits into PASS results.

| Workload | Current result | Exact boundary |
|---|---|---|
| Costa v1.8.0 | PASS on `full` | Desktop, one moving cursor, Calculator nested EXEC, stable observation. |
| DOSNavigator | PARTIAL | The upstream `DN.COM` is packaged unchanged and passes dual-pane startup, one-row arrows, mouse, Colors/XMS, native `Alt+X` exit, shell return, and same-boot runtime/mouse/video cleanup; wider file/editor and full-CD workflows remain open. |
| WOLF3D | PARTIAL | Menus and correctly rendered first level on `full`; injected page-flip patch, clean exit, full-CD, and audio remain open. |
| Original DOOM | PASS on `full` for bounded visual/audio workflow | Real gameplay plus objective non-silent AdLib music/PC-speaker SFX WAV pass; SB16/DMX, clean exit, full-CD, and real hardware are not proven. |
| doom-vanille | PARTIAL | Automatic 256 KiB low-DOS allocation, combined DMX OPL2/SB16 initialization, real gameplay, HUD/wall integrity, objective mixed output, and 350-gametic performance in 172 realtics pass after restoring the required Watcom ABI; audio-enabled clean exit and full-CD remain open. |
| Windows 3.1 | PASS on `full` for bounded Phase 9 workflow | Enhanced Mode start/restart, one linear 1:1 pointer, native SB startup WAV, objective non-silent `CANYON.MID` AdLib playback, generic `COMMAND.COM` DOS-VM return, unprofiled DOS/4GW Doom dynamic-memory launch/exit, Calculator `Alt+F4`, Program Manager survival, and clean DOS return. |
| mTCP/Crynwr | PASS on `full` for bounded Phase 8 workflow | Config persistence, resident ICMP, Internet Echo, authenticated FTP upload/download and persistence. |

## Release Gates

The release-facing validation set is:

```bash
bash scripts/build_full.sh
bash scripts/verify_phase5_runtime_ownership.sh --no-build
make qemu-test-all
make qemu-test-full-costa
make qemu-test-full-network-ftp
make qemu-test-full-network-icmp
make qemu-test-full-windows31
```

The aggregate includes every locally available bundled game and Windows workflow. Networking and the long installer gates remain separate; use the affected focused lane whenever shared DOS, memory, disk, input, video, audio, network, or setup code changes.

## Immediate Operational Order

1. Implement an opt-in monitored foreground DOS session with explicit lifecycle, virtual interrupts and cleanup while preserving existing boot/fullscreen paths.
2. Virtualize direct VGA/text memory, registers and focused keyboard input; supply a host renderer that can operate under the monitor.
3. Coordinate DPMI and audio/peripheral ownership, then qualify original Doom/Wolf binaries with actual video/audio, cleanup and separate physical T23/E500 evidence. Cooperative ports cannot satisfy this gate.
4. Preserve the historical DOS/Windows/audio gates while expanding the Phase 6 corpus and full-CD workflows; keep original Doom SB16/DMX and physical compatibility gaps explicit.
5. Extend networking and Windows compatibility under their existing separate phase gates.
