# Current Milestones

Validated through 2026-09-01, this is the current cross-phase status ledger for `CiukiOS pre-Alpha v0.7.1`. Dated investigation notes and files under `handoff/` remain historical evidence; when they disagree with this ledger, `Roadmap.md` and the current compatibility matrix govern.

## Phase Status

| Phase | Status | Reached milestone | Still required |
|---|---|---|---|
| 3 — DOS graphics/runtime | CLOSED | Stable shell-first graphics/runtime baseline. | Regression maintenance only. |
| 3.5 — Setup | CLOSED for functional `full` MVP | Packaged `SETUP.COM` and accepted installer flow. | Per-row B6 keyboard evidence and post-MVP media work remain backlog. |
| 4 — DOOM gameplay | CLOSED for its accepted 2026-05-04 scope | Original DOOM reached the recorded playable visual milestone on `full`. | This does not cover current game audio or doom-vanille rendering. |
| 5 — Runtime ownership | CLOSED | Loader-only Stage1 plus wholesale ABI2 CIUKIDOS owner on `full` and `full-cd`. | No placement tranche remains; behavioral compatibility continues in Phase 6. |
| 6 — DOS applications | ACTIVE | Bounded Costa and CuteMouse PASS workflows; stronger DOSNavigator, WOLF3D, doom-vanille, shell, process, editor, mouse, and driver evidence. | Ten-program `full` corpus, five-category `full-cd` corpus, complete JFT/SFT behavior, and removal/productization of title-specific hooks. |
| 7 — Legacy audio | NOT CLOSED | SB16 detection/init, DMA/IRQ, protected-mode IRQ/timer, and controlled WAD playback pass in project probes. | Non-silent DOOM-class game audio, second external workload, full-CD, and real-hardware evidence. |
| 8 — Networking | ACTIVE; first bounded milestone complete | Configurable IPv4, resident ARP/ICMP independent of FTP, outbound Internet ping, host ping over TAP, and bidirectional FTP on `full`. | DHCP-server evidence, more NICs, encrypted/common protocols beyond FTP, full-CD, and physical hardware. |
| 9 — Windows pre-NT | ACTIVE; first bounded milestone complete | Optional local Windows 3.1 reaches 386 Enhanced Mode twice with one pointer, Calculator/task `Alt+F4`, Program Manager survival, clean shell return, and relaunch. | Wider Windows 3.1 coverage, full-CD, audio/printing/devices, real hardware, Windows 95, and Windows 98. |
| 10 — Build/release | ACTIVE | Canonical full/full-CD build, focused aggregate, negative ABI, installer, application, networking, and Windows gates. | Broader CI/release automation and real-hardware matrix. |

## Canonical Runtime Boundary

- Stage0 loads 8 sectors from the 72-sector reserved Stage1 slot.
- Loader-only Stage1: 1,542 bytes at segment `0x0800`.
- `CIUKIDOS.SYS`: 43,254 bytes at segment `0x0900`; hard ceiling `0xA900` (43,264 bytes).
- ABI: version 2, 11 descriptors, 8 bytes per descriptor, capability mask `0x003F`, `CHAIN=0`.
- Four EXEC-state frames occupy `0x1400-0x1457`; Stage2 starts at `0x1480`.
- The ten-byte kernel margin is a live release constraint and must stay build-gated.

## Current Compatibility Decisions

1. Mouse support is one shared DOS/BIOS path, not an executable-name policy. Costa and DOSNavigator use the resident `INT 33h` service; Windows 3.1 uses the IBM PS/2 `INT 15h/AH=C2h` callback path. BIOS PS/2 clients exclusively own their cursor rendering, preventing the former double pointer.
2. COM/MZ placement is title-independent first-fit allocation. Doom-vanille's 256 KiB request and DOSNavigator's COM-to-MZ chain are regression workloads, not kernel name checks.
3. A COM process that launches another program must release unused paragraphs with DOS `INT 21h/AH=4Ah` before nested `AH=4Bh`. `NETSTART` now relocates its transient stack and follows this standard contract before starting `NE2000.COM`; the kernel contains no networking-program exception.
4. Canonical interactive QEMU runs use SDL through a verified X11/XWayland transport, explicit i8042, 256 MiB VM RAM, approximately 63 MiB XMS-visible memory, one Pentium III vCPU, and required KVM. Explicit `auto`, `tcg`, and `tcg-safe` modes remain diagnostic portability choices.
5. Windows 3.1 media, Doom, WOLF3D, DOSNavigator, and other non-redistributable payloads remain local and ignored. Their tests must SKIP or report missing payload honestly when absent.
6. A historical or narrow PASS never overrides a newer direct failure. The doom-vanille allocation gate is green, but corrupted planar gameplay and digital-silent SB16 output remain open failures.

## Validated Bounded Workflows

| Workload | Current result | Exact boundary |
|---|---|---|
| Costa v1.8.0 | PASS on `full` | Desktop, one moving cursor, Calculator nested EXEC, stable observation. |
| DOSNavigator | PARTIAL | Dual-pane startup, one-row arrows, mouse, Colors/XMS, `EXIT`, shell return; bounded packaged-loader hook and wider workflows remain open. |
| WOLF3D | PARTIAL | Menus and correctly rendered first level on `full`; injected page-flip patch, clean exit, full-CD, and audio remain open. |
| Original DOOM | Historical Phase 4 visual PASS; audio open | Accepted visual/playable milestone only; SB16/DMX game audio is not supported. |
| doom-vanille | PARTIAL | 256 KiB low-DOS allocation and DMX initialization pass; planar gameplay is corrupt and the captured audio stream is digital silence. |
| Windows 3.1 | PASS on `full` for bounded Phase 9 workflow | Enhanced Mode start/restart, one pointer, Calculator `Alt+F4`, Program Manager survival, clean DOS return. |
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

Game/audio and installer gates remain separate because they are longer or require optional local payloads. Use the affected lane whenever shared DOS, memory, disk, input, video, audio, or setup code changes.

## Immediate Operational Order

1. Recover at least ten external `full` workflows and five redistributable cross-category `full-cd` workflows for Phase 6.
2. Replace or formally productize the DOSNavigator/WOLF3D packaged compatibility hooks through general DOS/video behavior.
3. Fix doom-vanille planar rendering and classify clean exit before using it as an audio A/B target.
4. Close a real non-silent DOOM-class audio path and add a second external audio workload.
5. Extend networking beyond NE2000/plain FTP and add full-CD plus physical-hardware evidence.
6. Broaden Windows 3.1, then investigate Windows 95/98 without weakening the DOS compatibility gates.
