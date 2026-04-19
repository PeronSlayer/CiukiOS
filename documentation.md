# CiukiOS Documentation

## Purpose
This file is the central human-readable documentation hub for CiukiOS.
It complements the roadmap files, the full changelog, and the per-task handoff files under `docs/handoffs/`.

Maintenance rule:
1. Update this file whenever a completed task changes the externally visible project state, architecture, validation workflow, or milestone status.
2. Keep writing handoffs for major changes, but also merge the durable outcome here so the current project state stays easy to read without replaying every handoff.
3. Treat this file as the stable project narrative and the handoffs as the detailed session log.

## Project Summary
CiukiOS is an open source RetroOS built from scratch with the current north star of running real DOS software, then reaching a first real DOS DOOM milestone.

The current direction is:
1. Execute real `.COM` and `.EXE MZ` DOS binaries.
2. Expand DOS compatibility through deterministic test gates.
3. Grow protected-mode and DOS extender support incrementally.
4. Reach a reproducible boot-to-DOOM path.
5. Extend compatibility over time toward FreeDOS and pre-NT Windows software.

## Primary Index
1. Full changelog: [CHANGELOG.md](CHANGELOG.md)
2. High-level roadmap: [Roadmap.md](Roadmap.md)
3. Detailed DOOM roadmap: [docs/roadmap-ciukios-doom.md](docs/roadmap-ciukios-doom.md)
4. DOS 6.2 compatibility roadmap: [docs/roadmap-dos62-compat.md](docs/roadmap-dos62-compat.md)
5. Donation/support info: [DONATIONS.md](DONATIONS.md)
6. Collaboration/session context: [CLAUDE.md](CLAUDE.md)
7. Detailed handoff log: [docs/handoffs/README.md](docs/handoffs/README.md)

## Current Version
Current public version in README: `CiukiOS Alpha v0.8.7`

## Current Project State

### Boot and runtime
1. UEFI loader boots stage2 reliably.
2. Fallback path to `kernel.elf` remains available and tested.
3. Splashscreen rendering is integrated.
4. Stage2 provides a shell and DOS-like runtime surface.

### DOS execution baseline
1. `.COM` execution is active with PSP-style semantics.
2. `.EXE MZ` execution is active with relocation and regression coverage.
3. DOS termination/status flow is wired through `INT 20h`, `INT 21h AH=4Ch`, and one-shot status retrieval semantics.
4. Startup-chain support covers `CONFIG.SYS`, `AUTOEXEC.BAT`, and `.BAT` execution flow.

### Filesystem and compatibility
1. FAT-backed file handling is active.
2. FAT32 capability has advanced with mount metadata, hint-based allocation, dynamic directory growth, and edge-semantics gates.
3. INT21 priority-A compatibility is established and covered by dedicated regression gates.

### Video and desktop
1. The GOP/video stack supports mode selection, policy checks, and dynamic backbuffer management.
2. The desktop/UI path has layout hardening, overlay support, and non-interactive regression coverage.
3. Persistent video-mode configuration is supported through CMOS-backed boot configuration plus `VMODE.CFG` fallback.

### FreeDOS and optional payloads
1. A symbiotic FreeDOS runtime pipeline exists and is validated.
2. OpenGEM integration is optional and covered by dedicated tests.

### M6 protected-mode and DOS extender readiness
1. PMODE startup and transition contracts are instrumented and tested.
2. Protected-mode memory-domain overlap checks are in place.
3. The current smoke chain is:
   - `CIUKPM.EXE`
   - `CIUK4GW.EXE`
   - `CIUKDPM.EXE`
   - `CIUK31.EXE`
   - `CIUK306.EXE`
   - `CIUKLDT.EXE`
   - `CIUKMEM.EXE`
4. Current shallow DPMI coverage includes:
   - `INT 2Fh AX=1687h` host detection plus descriptor metadata
   - `INT 31h AX=0400h` get-version callable slice
   - `INT 31h AX=0306h` raw mode-switch bootstrap slice
   - `INT 31h AX=0000h` allocate-LDT-descriptors callable slice
   - `INT 31h AX=0501h` allocate-memory-block callable slice

### DOOM milestone baseline
1. The frozen first target is user-supplied shareware `DOOM.EXE` v1.9.
2. The primary IWAD expectation is user-supplied `DOOM1.WAD`.
3. `DOOM.WAD` is accepted only as a controlled alias to the same shareware dataset.
4. Expected runtime class is `DOS/4GW`-style.
5. Expected first video path is `VGA mode 13h`.
6. First milestone success checkpoint is main menu reachable.
7. Deterministic packaging/discovery baseline exists for `DOOM.EXE`, `DOOM1.WAD`, optional `DEFAULT.CFG`, and generated `DOOM.BAT` under `/EFI/CiukiOS`.
8. A staged boot-to-DOOM failure-taxonomy harness (`make test-doom-boot-harness`) classifies progress into `binary_found`, `wad_found`, `extender_init`, `video_init`, and `menu_reached` stages.
9. A VGA mode 13h compatibility scaffold is in place (shell `vga13` command + deterministic startup marker + `make test-vga13-baseline`) as the first step toward the real mode-13h draw path.
10. BIOS compatibility surface markers are emitted at boot for `INT 10h`, `INT 16h`, `INT 1Ah`, and `INT 2Fh` to make DOOM-startup dependencies greppable.
11. Minimal DOS-like mouse driver exposed via `INT 33h` through the stable `ciuki_services_t` ABI (`int33` pointer); mandatory subset covers `AX=0000h/0001h/0002h/0003h/0004h/0007h/0008h` with stage2-owned state, clipping, and a safe fallback when no host mouse input is wired. Live pointer input is provided by the stage2 PS/2 AUX driver on IRQ12 (`stage2/src/mouse.c`), with atomic delta drain into the INT 33h state on `AX=0003h` and automatic fallback to state-only mode when the AUX channel is absent. A minimal software cursor for mode 13h is exposed via `svc.mouse_draw_cursor_mode13` (append-only ABI slot). Smoke gate: `make test-mouse-smoke`.
12. OpenGEM GUI launcher (Phase 1): `shell_run_opengem_interactive()` centralizes the preflight (entry probe + FAT readiness) and dispatch to `shell_run()` for three entry surfaces — the `opengem` shell command, the `OPENGEM` desktop dock item (seventh launcher entry), and the `ALT+O` desktop shortcut. Emits the serial marker sequence `OpenGEM: boot sequence starting` → `OpenGEM: launcher window initialized` → `OpenGEM: exit detected, returning to shell`, with `OpenGEM: runtime not found in FAT, fallback to shell` on the graceful-fallback path. Runtime layout and entry-probe contract are documented in `docs/opengem-runtime-structure.md`. Smoke gate: `make test-opengem-smoke`.
13. BAT interpreter hardening (OpenGEM UX Phase 2): `shell_run_batch_file()` is now a stable subset of `COMMAND.COM` with per-frame state save/restore (`%0..%9` positional args, `@ECHO`-state machine, current-path tracking). Supported keywords: `REM`, `::`, `:label`, `@`, `ECHO OFF|ON|.`, `SET`, `PAUSE`, `SHIFT`, `CALL`, `GOTO [label|:EOF]`, `IF [NOT] {EXIST|"a"=="b"|ERRORLEVEL N} <cmd>`, plus `%%` and `%0..%9` in expansion. Serial marker vocabulary `[ bat ] enter|exit|line|call|return|goto|goto :eof|pause|shift|aborted max-steps` plus `gem.bat reached gemvdi invocation`. Limits: 256 lines, 128 labels, 2048 steps, 4 nested CALL depth, 10 argv slots. Contract documented in `docs/bat-interpreter.md`. Smoke gate: `make test-bat-interp`.
14. OpenGEM desktop scene integration (OpenGEM UX Phase 3): `shell_run_opengem_interactive()` now takes a stack-allocated `desktop_snapshot` of the launcher focus on entry and restores it on every exit path (preflight fail + normal return). `ui.h` exposes `ui_get_launcher_focus()`, `ui_set_launcher_focus()`, `ui_launcher_item_count()` as append-only accessors. The `OPENGEM` dock entry is rendered through a new `ui_launcher_display_for()` helper that applies a `[G]` text-mode facsimile glyph, while keeping the canonical `OPENGEM` action key intact for dispatch and tests. Serial markers (frozen): `[ ui ] opengem dock state saved: sel=<n>`, `[ ui ] opengem overlay active`, `[ ui ] opengem overlay dismissed, state restored`. Fallback modal line `OPENGEM: n/a - payload not installed` is printed to the text console when the preflight fails, with the prior launcher selection restored. Smoke gate: `make test-opengem-launch`.
15. OpenGEM app discovery and file catalog (OpenGEM UX Phase 4): new module `stage2/src/app_catalog.c` (+ `stage2/include/app_catalog.h`) joins two discovery lanes — a FAT directory scan of `/`, `/FREEDOS`, `/FREEDOS/OPENGEM`, `/EFI/CiukiOS` for `.COM`/`.EXE`/`.BAT`, and the loader-provided `handoff->com_entries[]`. Entries are stored in a static, case-insensitively-deduped 256-slot array with the append-only shape `{char name[13]; char path[64]; u8 kind; u8 source; u8 reserved[2];}`. FAT wins on collision (users can override bundled demos). `stage2.c` calls `app_catalog_init(handoff)` after FAT mount. New shell command `catalog` lists the joined view; also advertised in `help`. Serial marker vocabulary: `[ catalog ] scan begin root=<path>`, `[ catalog ] scan entry <name> kind=<com|exe|bat> path=<path>`, `[ catalog ] scan done entries=<n> roots=<m>`. Smoke gate: `make test-opengem-file-browser`.
16. OpenGEM input routing and mouse/keyboard bridge (OpenGEM UX Phase 5): `stage2/include/mouse.h` gains an append-only `int33_hooks_t` ABI (`version` + `on_session_enter` + `on_session_exit` + `on_mouse_event`) plus four new functions: `stage2_mouse_set_opengem_hooks()`, `stage2_mouse_opengem_session_enter()`, `stage2_mouse_opengem_session_exit()`, `stage2_mouse_opengem_cursor_quiesced()`. `shell_run_opengem_interactive()` now brackets the `shell_run()` call with session_enter/exit so the CiukiOS fallback mode-13 cursor is suppressed while OpenGEM owns the screen; `shell_mouse_draw_cursor_mode13()` consults `opengem_cursor_quiesced()` before painting. The ALT+G+Q chord in `desktop` emits an additional `[ kbd ] opengem escape chord: alt+g+q detected` marker for boot-log correlation. Frozen serial markers: `[ mouse ] opengem session: cursor disabled`, `[ mouse ] opengem session: cursor restored`, `[ mouse ] opengem hook installed`, `[ kbd ] opengem escape chord: alt+g+q detected`. Smoke gate: `make test-opengem-input`.
17. OpenGEM DOOM path readiness (OpenGEM UX Phase 6): `stage2.c` probes the Phase 4 app catalog for user-supplied DOOM fixtures after initialization and emits `[ doom ] catalog discovered DOOM.EXE at <path>` / `[ doom ] catalog discovered DOOM1.WAD at <path>` when present (no-op for empty/license-compliant images). `shell_run_from_fat()` emits `[ doom ] opengem launch DOOM.EXE` on a case-insensitive match of the target name, giving telemetry a clear launch boundary. New harness `scripts/test_doom_via_opengem.sh` + Makefile target `test-doom-via-opengem` chains static invariants (always) and runtime boot-log assertions (fixture-gated via `CIUKIOS_DOOM_FIXTURES_DIR` + optional `CIUKIOS_DOOM_BOOT_LOG`); SKIPs cleanly when fixtures absent. Flow diagram + gap list: `docs/boot-to-doom-via-opengem.md`. Compatibility gaps (DOS/4GW extender, SoundBlaster emulation, event-driven INT 33h mode 0x0C, VGA scrolling corners, FAT write-through) are documented with forwards to their tracking roadmaps. DOOM binaries/WADs remain user-supplied; project does not redistribute.
18. OpenGEM full-runtime observability (OPENGEM-007): `shell_run_opengem_interactive()` now emits four granular, ordered runtime markers that separate a preflight-only pass from a real desktop-visible session — `OpenGEM: runtime handoff begin`, `OpenGEM: desktop first frame presented`, `OpenGEM: interactive session active` (emitted between the mouse session_enter and the `shell_run()` dispatch), and `OpenGEM: runtime session ended` (between `shell_run()` return and the mouse session_exit). All historical markers (OPENGEM-001/-003/-005) are preserved for backward-compat. New gate `scripts/test_opengem_full_runtime.sh` + Makefile target `test-opengem-full-runtime` enforces both the static presence of the markers and the emission ordering via two AWK probes (14 OK / 0 FAIL), with an opt-in runtime boot-log probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-full-runtime-validation.md`. No ABI break; no version bump (`Alpha v0.8.7`).
19. OpenGEM real first-frame hook + session duration (OPENGEM-008, refined by OPENGEM-009): `stage2/include/gfx_modes.h` gains an append-only arm/disarm ABI — `gfx_mode_opengem_arm_first_frame()`, `gfx_mode_opengem_disarm_first_frame()`, `gfx_mode_opengem_first_frame_armed()`. `gfx_mode_present` in `stage2/src/gfx_modes.c` emits the frozen marker `OpenGEM: desktop frame blitted` exactly once after the first successful `gfx_mode13_present_plane()` call while armed, then auto-disarms. The cached no-op branch never triggers the marker, so the signal is tied to a genuine mode-13 upscale into the backbuffer. `shell_run_opengem_interactive()` brackets the `shell_run()` dispatch with arm/disarm and emits `OpenGEM: runtime session duration=<n> ms` (OPENGEM-009) between `OpenGEM: runtime session ended` and `stage2_mouse_opengem_session_exit()`; duration is wall-clock milliseconds derived from `stage2_timer_ticks()` delta × 10 (PIT at 100 Hz). The marker prefix `OpenGEM: runtime session duration=` is stable across OPENGEM-008/009; only the suffix moved from ` frames` to ` ms`. Gate: `scripts/test_opengem_real_frame.sh` + Makefile target `test-opengem-real-frame` (21 OK / 0 FAIL). Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-real-frame-validation.md`. No ABI break; no version bump (`Alpha v0.8.7`).
20. OpenGEM dispatch-target telemetry (OPENGEM-010): `shell_run_opengem_interactive()` now probes `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` before `/FREEDOS/OPENGEM/GEM.BAT`, bypassing the stock GEM.BAT's drive-root check that prevented the real GEM binary from being reached under the CiukiOS FAT layout. Immediately before `shell_run()` dispatch, the runtime emits `OpenGEM: dispatch target=<absolute path> kind=<bat|exe|com|app>`, giving a runtime gate a verifiable correlation between the selected probe path and the downstream OPENGEM-008/009 markers (`desktop frame blitted` + `runtime session duration=<n> ms`). `kind` is inferred from the trailing 3 characters of the resolved path, ASCII-folded to lowercase. New gate `scripts/test_opengem_dispatch.sh` + Makefile target `test-opengem-dispatch` (7 OK / 0 FAIL) enforces the new probe ordering, marker prefix/kind tokens, and emission order arm→dispatch→`shell_run`. Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-dispatch-telemetry.md`. No ABI break; no version bump (`Alpha v0.8.7`).
21. OpenGEM DOS extender readiness probe (OPENGEM-011): `stage2/src/shell.c` gains `stage2_opengem_probe_extender()`, a readiness helper that synthesizes the DPMI installation-check register file (`AX=1687h`, carry set) and invokes the in-process `shell_com_int2f()` handler directly to capture the DPMI-stub surface without dispatching a real interrupt. Emits a frozen append-only marker set between the OPENGEM-010 dispatch marker and `shell_run()`: `OpenGEM: extender probe begin`, `OpenGEM: extender dpmi installed=<0|1> flags=0x<hex16>`, `OpenGEM: extender mode=<dpmi-stub|none>`, `OpenGEM: extender probe complete`. The `flags` word packs installed + nonzero CX (host-data size) + nonzero ES (entry seg) + nonzero DI (entry offset). Return value is advisory; actual protected-mode dispatch lands in OPENGEM-012+. Gate `scripts/test_opengem_extender.sh` + Makefile target `test-opengem-extender` (13 OK / 0 FAIL) enforces sentinel, probe invocation shape, frozen marker set, both mode branches, invocation ordering `dispatch→probe→shell_run`, and first-occurrence marker order `begin<installed<mode<complete`. Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-extender-readiness.md`. No ABI break; no version bump (`Alpha v0.8.7`).
22. OpenGEM absolute-dispatch classification (OPENGEM-012): `stage2/src/shell.c` gains `stage2_opengem_classify_absolute(path, size)` + `shell_write_u32_hex()`. The classifier reuses the FAT directory-entry size already captured by the preflight (new `found_size = probe.size`) and publishes a capability verdict for the resolved absolute path using only the trailing 3 chars of the extension. Emits a frozen append-only marker set between the OPENGEM-011 extender probe and `shell_run()`: `OpenGEM: absolute dispatch begin path=<p> size=0x<hex32>`, `OpenGEM: absolute dispatch classify=<mz|bat|com|app|unknown> by=path`, `OpenGEM: absolute dispatch capable=<0|1> reason=<token>`, `OpenGEM: absolute dispatch complete`. Reason tokens are a stable contract: `16bit-mz-extender-pending`, `bat-interp-available`, `com-runtime-available`, `no-loader-for-app`, `unknown-extension`, `no-path`. The `capable` flag is advisory; consumers still go through `shell_run()` until OPENGEM-013+ promotes it to a gate. Gate `scripts/test_opengem_absolute_dispatch.sh` + Makefile target `test-opengem-absolute-dispatch` (24 OK / 0 FAIL) enforces sentinel, helper presence, all four marker prefixes with their tokens, all five classify labels, all six reason tokens, invocation ordering `extender→classify→shell_run`, first-occurrence marker order `begin<classify<capable<complete`, and preflight size capture. Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-absolute-dispatch-classify.md`. No ABI break; no version bump (`Alpha v0.8.7`).
23. OpenGEM absolute-path preload probe (OPENGEM-013): `stage2/src/shell.c` gains `stage2_opengem_preload_absolute(path, expect_size, classify)`. First CiukiOS-side code path that actually calls `fat_read_file()` on the absolute path resolved by OPENGEM-010, staging the binary into the runtime payload buffer at `SHELL_RUNTIME_COM_ENTRY_ADDR` (size guards: `preload-empty`, `preload-too-large`, `preload-io-error`, `preload-no-path`). Inspects the first two bytes to publish a real on-disk signature (`MZ`, `ZM`, `text`, `empty`, `unknown`), cross-checks it against the OPENGEM-012 classify label, and emits a dispatch verdict. Frozen append-only marker set emitted between the OPENGEM-012 classify probe and `shell_run()`: `OpenGEM: preload begin path=<p> expect_size=0x<hex32>`, `OpenGEM: preload read bytes=0x<hex32> status=<ok|too-large|io-error|no-path>`, `OpenGEM: preload signature=<MZ|ZM|text|empty|unknown> match=<0|1>`, `OpenGEM: preload verdict=<dispatch-native|defer-to-shell-run> reason=<token>`, `OpenGEM: preload complete`. Ten stable verdict-reason tokens (disjoint from OPENGEM-012): `preload-empty`, `preload-too-large`, `preload-io-error`, `preload-no-path`, `signature-mismatch`, `mz-16bit-pending`, `bat-interp-ready`, `com-runtime-ready`, `unsupported-app`, `unsupported-unknown`. Verdict literal `dispatch-native` is reserved for OPENGEM-014 but is already a public contract token. Execution still defers to `shell_run()`; the helper returns 1/0 advisory. Gate `scripts/test_opengem_preload.sh` + Makefile target `test-opengem-preload` (37 OK / 0 FAIL). Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-preload.md`. No ABI break; no version bump (`Alpha v0.8.7`).
24. OpenGEM native absolute-path dispatcher (OPENGEM-014): promotes OPENGEM-013 from observability to real execution. `stage2_opengem_preload_absolute()` signature extended with `out_verdict`/`out_reason`/`out_read_bytes` so the caller can branch without re-parsing serial. Bat and com paths now emit `verdict=dispatch-native` (reasons `bat-interp-ready` and `com-runtime-ready` unchanged). New helper `stage2_opengem_dispatch_native(boot_info, handoff, path, read_bytes, verdict, reason)` in `stage2/src/shell.c` consumes the verdict and invokes the interpreter/runtime directly on the resolved absolute path: BAT → `shell_run_batch_file(boot_info, handoff, path)`; COM → `shell_run_staged_image(boot_info, handoff, basename, read_bytes, "")` on the already-staged buffer (eliminates the double-I/O risk flagged in OPENGEM-013). MZ / signature-mismatch / unsupported-* / preload-* keep `defer-to-shell-run` and fall through to the historical `shell_run()` dispatcher. Frozen append-only marker set (disjoint from preload): `OpenGEM: native-dispatch begin path=<p> kind=<bat|com> reason=<r>`, `OpenGEM: native-dispatch <kind>=<invoked|failed>`, `OpenGEM: native-dispatch complete errorlevel=<n>`. `shell_run_opengem_interactive()` now branches on the dispatcher's return: native dispatch skips `shell_run()`, else the historical path runs. Gate `scripts/test_opengem_native_dispatch.sh` + Makefile target `test-opengem-native-dispatch` (20 OK / 0 FAIL) enforces sentinel, helper presence, out-param plumbing, all five dispatcher marker variants, real execution calls (`shell_run_batch_file`/`shell_run_staged_image`), verdict promotion for bat/com, MZ defer preservation, call-site ordering `preload→dispatch_native→shell_run(else)`, and internal marker ordering (serial_write lines only) `begin<(bat|com)=<complete`. Regression stack now 15 gate, all PASS. Contract document: `docs/opengem-native-dispatch.md`. Note: `gem.exe` (16-bit MZ) remains intentionally on the defer path — real native dispatch for 16-bit code requires a v8086 monitor or DPMI server, scoped to OPENGEM-016+. No ABI break; no version bump (`Alpha v0.8.7`).
25. OpenGEM MZ deep-header probe (OPENGEM-015): `stage2/src/shell.c` gains `stage2_opengem_mz_probe(path, preload_size)`. Parses the full 28-byte MZ header already staged at `SHELL_RUNTIME_COM_ENTRY_ADDR` by OPENGEM-013 (zero I/O — reuses the preload buffer), surfaces all 12 header fields (`e_cblp`, `e_cp`, `e_crlc`, `e_cparhdr`, `e_minalloc`, `e_maxalloc`, `e_ss`, `e_sp`, `e_cs`, `e_ip`, `e_lfarlc`, `e_ovno`), computes canonical load size (`file_bytes = e_cp*512 - (512-e_cblp) if e_cblp!=0; load_bytes = file_bytes - e_cparhdr*16`), and publishes a viability verdict that promotes the "needs-extender" gap from a shell_run-side rejection string to a first-class marker. Gated on `classify_label=="mz"` so non-MZ paths don't pollute the stream. Invoked after OPENGEM-013 preload, before OPENGEM-014 dispatcher. Frozen append-only marker set (10 markers, disjoint from preload/native-dispatch): `OpenGEM: mz-probe begin path=<p> size=0x<hex32>`, `signature=<MZ|ZM|none> status=<ok|too-small|not-mz>`, `header e_cblp=… e_cp=… e_crlc=… e_cparhdr=…`, `alloc e_minalloc=… e_maxalloc=…`, `stack e_ss=… e_sp=…`, `entry e_cs=… e_ip=…`, `reloc e_lfarlc=… e_ovno=…`, `layout load_bytes=0x<hex32> header_bytes=0x<hex32>`, `viability=<runnable-real-mode|requires-extender|malformed|skipped-non-mz> reason=<token>`, `complete`. Viability ladder: load>0xA0000 → `requires-extender reason=mz-load-exceeds-real-mode`; `e_maxalloc==0xFFFF && load>0x10000` → `requires-extender reason=mz-max-alloc-64k`; else → `runnable-real-mode reason=mz-v8086-candidate`. Seven stable reason tokens: `mz-v8086-candidate`, `mz-load-exceeds-real-mode`, `mz-max-alloc-64k`, `mz-header-too-small`, `mz-header-malformed`, `mz-non-mz-skipped`, `mz-no-buffer`. No execution change — MZ still routes through `shell_run()` and gets the historical `[dosrun] mz dispatch=pending reason=16bit` rejection. Gate `scripts/test_opengem_mz_probe.sh` + Makefile target `test-opengem-mz-probe` (41 OK / 0 FAIL). Opt-in runtime probe via `CIUKIOS_OPENGEM_BOOT_LOG`. Contract document: `docs/opengem-mz-probe.md`. Regression stack now 17 gate, all PASS. No ABI break; no version bump (`Alpha v0.8.7`). OPENGEM-016+ must deliver a 16-bit execution layer (v8086 monitor or DPMI server) to unblock real native dispatch of gem.exe; this is the first OpenGEM milestone that cannot be closed in a single session.

## Important Repository Files

### Root files
1. `README.md`: public-facing project overview and latest two changelog entries.
2. `CHANGELOG.md`: complete release/change history.
3. `Roadmap.md`: high-level roadmap and current milestone state.
4. `documentation.md`: central durable documentation for current project state.
5. `CLAUDE.md`: collaboration workflow and session rules.
6. `DONATIONS.md`: support and donation details.

### Core source areas
1. `boot/uefi-loader/`: UEFI loader.
2. `stage2/`: main runtime, shell, FAT, UI, video, and compatibility code.
3. `kernel/`: fallback kernel path.
4. `com/`: DOS smoke binaries and focused runtime probes.
5. `scripts/`: validation, orchestration, and packaging test scripts.
6. `third_party/`: imported external runtime payloads and related assets.

### Documentation areas
1. `docs/roadmap-ciukios-doom.md`: detailed DOOM-oriented execution plan.
2. `docs/roadmap-dos62-compat.md`: DOS 6.2 compatibility track.
3. `docs/int21-priority-a.md`: INT21 compatibility baseline and matrix.
4. `docs/handoffs/`: dated change handoffs for major tasks.

## Test and Validation Overview
The project emphasizes deterministic, scriptable validation.

On graphical Linux hosts where headless QEMU boots do not emit usable serial markers,
the stage2 and dosrun runtime gates now retry automatically with a graphical QEMU
fallback while keeping serial capture enabled. This preserves a deterministic log-based
result on hosts where `CIUKIOS_QEMU_HEADLESS=1` is silent, instead of failing with an
unclassified infrastructure-only outcome.

The M6 DOS-extender readiness chain now also covers a stateful DPMI memory-release
slice: `CIUKMEM.EXE` validates `INT 31h AX=0501h` allocation shape, while
`CIUKREL.EXE` validates a successful `AX=0502h` free against a real prior handle and
an invalid-handle rejection on duplicate free.

Important gates include:
1. `make test-stage2`
2. `make test-fallback`
3. `make test-phase2`
4. `make test-video-mode`
5. `make test-video-1024`
6. `make test-video-ui-v2`
7. `make test-m6-pmode`
8. `make test-m6-smoke`
9. `make test-m6-dos4gw-smoke`
10. `make test-m6-dpmi-smoke`
11. `make test-m6-dpmi-call-smoke`
12. `make test-m6-dpmi-bootstrap-smoke`
13. `make test-m6-dpmi-ldt-smoke`
14. `make test-m6-dpmi-mem-smoke`
15. `make test-vga13-baseline`
16. `make test-doom-target-packaging`
17. `make test-doom-boot-harness`
18. `bash scripts/test_doom_readiness_m6.sh`

## Current Milestone Gaps
The main remaining gaps before the first DOOM milestone are:
1. Replace the current shallow bootstrap smoke ceiling with a more realistic DOS extender execution target.
2. Harden high-memory/protected-mode handoff for real extender behavior.
3. Close the remaining BIOS and DOS runtime gaps actually used by target startup.
4. Implement `VGA mode 13h` compatibility sufficient for a real frame/menu checkpoint.
5. Extend the current packaging baseline into a staged boot-to-DOOM runtime harness.
6. Add gameplay-relevant input and audio compatibility.

## Documentation Workflow
When a meaningful task is completed:
1. Update the relevant roadmap/changelog files if public status changed.
2. Write a handoff in `docs/handoffs/` for major multi-file or architectural changes.
3. Update this file if the task changed the stable project state, architecture, tests, or milestone progress.

This keeps three layers aligned:
1. `CHANGELOG.md` for release history.
2. `documentation.md` for current durable project state.
3. `docs/handoffs/` for detailed implementation history.