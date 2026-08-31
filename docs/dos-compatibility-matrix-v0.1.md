# DOS Compatibility Matrix v0.1

## Status

Phase 5 is closed on the wholesale loader/kernel ownership boundary described below. Phase 6 is active and not closed: current evidence proves useful runtime slices and a small number of external workflows, but the required ten-program `full` corpus and five-category redistributable `full-cd` corpus are incomplete. Complete DOS-compatible JFT/SFT behavior remains backlog and is not implied by the Phase 5 result.

## Phase 5 Closure Baseline

| Boundary | Canonical value |
|---|---|
| Stage1 | Loader-only, 1,542 bytes at `0x0800`; stage0 loads 8 sectors while the reserved BPB/disk slot remains 72 sectors. |
| CIUKIDOS | Wholesale DOS kernel at `0x0900`; `ABI=2`, 11 services, 8-byte descriptors, capability mask `0x003F`, and `CHAIN=0`. |
| Kernel size | 43,254-byte current artifact; hard maximum `0xA900` (43,264 bytes). |
| Low-memory ownership | Four EXEC-state frames at `0x1400-0x1457`; Stage2 at `0x1480`. |

The same-checkout closure evidence passes static ownership verification, all eight fatal ABI2 negative cases, `full` and `full-cd` boot with the deterministic high-LBA read, nested COM/MZ and TSR restoration, setup from direct CD to standalone HDD boot, CuteMouse, the focused doom-vanille and WOLF boundaries, DRVLOAD smoke, and repeated shell stability. These PASS results establish the Phase 5 ownership boundary only; they do not add internal probes or partial workflows to the Phase 6 external corpus.

## Evidence Rules

1. PASS requires a repeatable lane or an accepted historical evidence bundle and covers only the named workflow; a current-checkout or release claim requires a fresh rerun from that same checkout.
2. A launch banner is launch evidence, not an interactive-workflow PASS.
3. Internal CiukiOS probes do not count toward the external-application exit corpus.
4. A locally patched third-party binary must be reported as PARTIAL until the underlying compatibility gap is fixed or the patch becomes an explicitly accepted product policy.
5. Full evidence does not imply full-CD D: evidence.
6. PARTIAL, skipped, deferred, and payload-missing results never count as PASS.

## Status Legend

- **PASS**: the documented workflow is repeatable without a compatibility workaround.
- **PARTIAL**: a useful boundary is reached, but meaningful behavior, portability, or clean completion remains blocked.
- **PASS (internal proxy)**: repeatable project-owned evidence that does not count as an external compatibility result.
- **FAIL**: the required workflow reaches a reproducible defect.
- **NOT RUN**: no current same-checkout result exists, or a required local payload is unavailable.

## Current Evidence Matrix

| Workload | Category | Status | Proven scope | Open boundary |
|---|---|---|---|---|
| `CIUKRTST.COM` | Runtime ownership probe | PASS (internal proxy) | A normal COM child confirms resident `INT 21h` ownership in segment `0x0900`, `ABI=2`, 11 services, 8-byte descriptors, capability mask `0x003F`, `CHAIN=0`, and coherent DOS version/default-drive/PSP/DTA state. Current marker: `[CIUKRTST] OWNER=CIUKIDOS ABI=2 SERVICES=11 CHAIN=0 STATE=PASS`. The current kernel is 43,254 bytes with maximum `0xA900`; EXEC frames are at `0x1400-0x1457` and Stage2 is at `0x1480`. | Project-owned black-box probe; it does not count toward the external Phase 6 corpus or prove complete JFT/SFT semantics. |
| `CIUKPST.COM` | Process/TSR ownership probe | PASS (internal proxy) | Current `full` evidence records nested COM/MZ restoration, `MODE=31 RESIDENCY=PASS EXEC-SURVIVE=PASS UNLOAD=PASS`, final `TSR-EXEC-UNLOAD=PASS`, and `/ROOT` restoration. | Project-owned full-profile evidence; it does not count toward the external corpus or prove arbitrary TSR/full-CD compatibility. |
| `SHELL.COM` + COM/MZ probes | Command processor / execution | PASS (internal proxy) | External default shell, absolute and relative multi-component COM/MZ execution, PATH lookup, child return, and file-operation regression surface on `full`; the full-CD D: lane also passes COM/MZ, CIUKRTST/PSTACK, and a deterministic FAT16 read beyond LBA 65,535. | Internal probes; the full-CD evidence is not a general external-app workflow. |
| `CIUKEDIT.COM` / `EDIT` | Full-screen text editor | PASS (internal proxy) | VGA 80x25 workbench UI, dynamic 32 KiB document allocation, existing/new file handling, multiline insert/overwrite editing, line/column gutter and scrolling, arrows/Home/End/PgUp/PgDn navigation, Backspace/Delete/Tab, save/save-as, dirty-exit protection, help overlay, DOS CRLF persistence, marker completion, and prompt return on `full`. `EDIT` resolves to `C:\APPS\CIUKEDIT.COM`. | Project-owned editor; it does not count as the external editor required by the Phase 6 corpus. |
| `GFXSTAR` | Graphics utility | PASS (internal proxy) | Project-owned graphics path emits `[GFXSTAR] PASS`, returns to text mode, and recovers the external-shell prompt in the DOS compatibility smoke. | Project-owned behavior, not a third-party application. |
| DOSNavigator `DN.COM` | External file manager/editor | PARTIAL | The optional local payload chains from its COM stub into the MZ payload, renders the complete dual-pane UI on `full`, moves exactly one row per arrow key, accepts PS/2 mouse input, completes the Colors/XMS action, handles `EXIT`, and restores the CiukiOS prompt without an allocation/runtime failure. | The packaged loader still carries a bounded compatibility hook; no broad file-operation/editor workflow or equivalent full-CD result is recorded. |
| Costa v1.8.0 | External graphical desktop/application suite | PASS on `full` for bounded workflow | Pinned MIT payload launches from any cwd, renders the 640x350 desktop with one moving cursor, opens the complete Calculator UI through the three-process MZ chain, and remains stable through the observation point. | No full-CD lane; only Calculator is covered, and the gate does not prove every Costa application or input workflow. |
| CuteMouse `CTMOUSE.EXE` | External command-line driver/utility | PASS on `full` | The isolated GPL workflow installs with `/N /P`, exposes the CuteMouse 7.05 INT 33h identity, unloads with `/U`, restores the exact baseline handler tuple, returns to the prompt, and leaves the canonical image unchanged (`QEMU_RC=0`). | No equivalent full-CD D: workflow is recorded; this PASS covers only the named install/query/unload workflow. |
| WOLF3D | External real-mode game | PARTIAL | The current `full` taxonomy reaches `binary_found`, `exec_attempted`, `transfer_marker`, `runtime_stable`, and `visual_gameplay`; its fresh first-level frame is `640x400` with more than 100 sampled colors. The automated workflow reaches Options/New Game, episode/difficulty selection, a correctly rendered first level, submits gameplay input, and remains stable. | The packaged copy requires an injected page-flip/VGA Attribute Controller compatibility patch; unmodified-binary, clean-exit, full-CD, and audio compatibility are not proven. Affected QEMU 11.1 TCG builds require KVM or the slower `one-insn-per-tb` fallback for the self-modifying renderer. |
| Original DOOM | External DOS-extender game | PASS for Phase 4 visual scope | Optional local payload reaches the recorded DOS/4GW, WAD, video, and playable visual milestone on `full`. | SB16/DMX audio is still failing; this row is not an audio PASS and does not prove full-CD execution. |
| doom-vanille / `PCDOOM.EXE` | Rebuildable DOS-extender application | PARTIAL | The isolated `full` low-memory lane reaches DOS/4GW, DPMI memory initialization, and `ST_Init`, stays alive after the startup marker, and passes `[doomvan-memory] PASS sb16_low_dos_256k`; the former `I_AllocLow(256000)` failure is no longer reproduced. | This is only the 256 KiB low-DOS/startup gate: gameplay, meaningful visual interaction, audio behavior, clean application exit, and full-CD execution remain unproven. |
| `DOOMSFX` | Controlled protected-mode audio probe | PASS (internal proxy) | Loads selected WAD lumps and completes controlled SB16 DMA/IRQ playback markers. | Project-owned harness; does not prove original DOOM/DMX or a second external audio workload. |
| `DRVLOAD.COM` + `QCDROM.SYS` | Driver/helper compatibility | PARTIAL | Packaging, the current DRVLOAD smoke and repeated shell-stability sequence pass; native `.SYS` INIT and QEMU CD-device detection as `QCDROM1` also have evidence. | Activation remains an evidence/helper lane rather than productized runtime policy, and full JFT/SFT integration is still backlog. |
| Crynwr `NE2000.COM` + CiukiOS resident bridge + mTCP | External networking / file transfer | PASS on `full` for bounded Phase 8 scope | `NETSTART` owns physical `INT 60h`, exposes mTCP on `INT 61h`, and answers ARP/ICMP independently of FTP. `NETCFG` persists IP/mask/gateway/DNS; gates prove inbound Echo without `FTPSRV`, outbound Internet Echo, and authenticated FTP list/download/upload through `C:\SHARE`. | FTP remains plaintext; DHCP-server interoperability, other NICs, physical networks beyond Linux TAP/NAT, encrypted protocols, SMB/CIFS, and full-CD are not proven. |
| Windows 3.1 386 Enhanced Mode | External pre-NT operating environment | PASS on `full` for bounded Phase 9 workflow | Optional local media reaches Program Manager twice at 640x480 through `WIN`; the lane proves exactly one moving PS/2 pointer, Calculator launch, task-scoped `Alt+F4`, Program Manager survival, Windows exit, visible CiukiOS shell recovery, and relaunch. | Proprietary payload remains local/untracked. Full-CD, broader applications/devices/audio/printing, real hardware, Windows 95, and Windows 98 are not proven; this row does not count as a DOS Phase 6 corpus item. |
| MSCDEX with `QCDROM1` | External redirector/device integration | PARTIAL | Historical focused evidence records MSCDEX child success and a visible `Drive A: = Driver QCDROM1 unit 0` mapping. | The `A:` mapping is not an acceptable product drive-letter policy; current full/full-CD reproducibility and runtime integration must be revalidated. |
| `SETUP.COM` | Installer/setup utility | PASS for closed MVP scope | Current evidence includes setup packaging, a read-only direct-CD/blank-HDD probe, full direct-CD cloning to a disposable HDD, partition-aware D:→C: Stage1 patching, and autonomous boot at `C:\APPS`. | The keyboard contract still has sampled rather than per-row evidence; arbitrary third-party setup compatibility is not implied. |

Commercial or otherwise non-redistributable payloads remain optional, local, and untracked. Their absence must produce SKIP/NOT RUN rather than a false PASS.

Acknowledgement for the optional packaged utility: "Based on Dos Navigator by RIT Research Labs."

## Phase 6 Exit Matrix

The closure corpus must contain at least ten external programs on `full`, with at least two from each category:

1. command-line utilities
2. editors or file managers
3. real-mode games
4. DOS-extender applications
5. installers or setup tools

For every application, the recorded workflow must include:

1. discovery and launch
2. at least one meaningful category-specific action
3. expected file, console, graphics, or device behavior
4. termination or a documented bounded observation point
5. clean shell/runtime recovery where the application supports exit

At least five redistributable representatives spanning all five categories must run through an equivalent matrix from the `full-cd` D: environment.

Phase 6 can close when:

1. at least 80% of required matrix cases are PASS
2. 100% of non-PASS cases identify the failing subsystem and a reproducer
3. no critical open defect corrupts memory, storage, handles, parent process state, or shell recovery
4. no title-specific injected patch is counted as general compatibility
5. aggregate and affected focused gates are green on the same checkout

## Immediate Expansion Order

1. Keep the doom-vanille 256 KiB low-DOS regression green and extend it to meaningful video/gameplay, audio classification, and clean completion.
2. Extend DOSNavigator beyond its validated navigation/mouse/Colors/exit workflow into broad file operations and a full-CD lane, then remove or formally productize its bounded loader hook.
3. Replace the now-explained WOLF3D VGA Attribute Controller workaround with a general video/timer compatibility fix, then prove the unmodified binary.
4. Add another external command-line utility and an external editor with redistributable test payloads.
5. Carry CuteMouse and four additional redistributable category representatives into the five-workload full-CD D: matrix.
6. Complete and independently gate full JFT/SFT behavior without treating the closed loader/kernel placement boundary as evidence that handle internals are complete.
