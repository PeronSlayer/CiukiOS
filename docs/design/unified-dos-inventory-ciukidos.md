# CiukiDOS inventory for the unified DOS

f3-54, 2026-10-21. Source baseline `cda00dfbb2b072be755c66120fa1c17d87c2ab4e`.
This inventories source behavior and port obligations, not measured game
compatibility. Read with the [contract](unified-dos-contract.md). No source in
`legacy/local/` was consulted. Line numbers below are baseline coordinates.

Classification: **U** upstream-equivalent, retain/test; **K** port behavior
into the FreeDOS fork/kernel hooks; **D** Ciuki-specific CONFIG.SYS driver or
shell/guest helper; **R** retire from unified runtime for the stated reason,
preserving the old selectable guest until the retirement gate. The native
VMM remains the owner of its existing services.

## INT21 implementation and the active handoff

The legacy dispatch is `src/boot/floppy_stage1.asm:927–1145`; the active
canonical personality is assembled by `src/runtime/ciukidos.asm` with
`CIUKIDOS_KERNEL_BUILD`. The f3-50 handoff runs before the legacy prologue.
`src/kernel/vm/dos_file.c:440–450` is the active file-selector oracle.
These are distinct inventories: absence in legacy dispatch does not mean
absence in the bound canonical guest.

| INT21 selectors | Source/status in CiukiDOS | Class and unified destination |
| --- | --- | --- |
| 00,20,4C,4D | Legacy termination/child status, dispatch and EXEC contexts | U+K: FreeDOS termination, exact lifecycle/ref rollback; keep normal/abort/TSR codes. |
| 01,02,06–0C | Legacy console and keyboard; `ciukidos_read_key:915`, dispatch `:1011–1028` | U+K: FreeDOS chario and idle hooks. Blocking waits must yield. |
| 03–05 | No matching legacy dispatch branch; older prose claiming complete 00–0C overstates it | U: upstream AUX/PRN support is new behavior, append probes; do not remove devices. |
| 0D,0E,19,1B,1C,36 | Legacy drive/allocation routines `:5059,5197,5422,5449`; active VMM-owned | K: flush/drive/CDS/allocation hooks around local IFS. |
| 0F–17 | No legacy FCB dispatch; f3-50 active `dos_file_fcb.inc` provides open/close/find/delete/sequential/create/rename | U+K: upstream FCB engine over the same bound SFT; preserve f3-50 statuses and checks. |
| 18,1D–1F | No legacy branch; unassigned/old functions are not missing new APIs | U: preserve documented upstream behavior; no invented success. |
| 1A,2F | DTA set/get `:5047,6435`; active VMM bookkeeping | K: real FreeDOS DTA field plus per-process native association, changed once. |
| 21–24,27,28 | No legacy FCB dispatch; active adapter implements random I/O/size/set-random/block I/O | U+K: upstream FCB SFT path plus guarded boundary/overflow/partial-record behavior. |
| 25,35 | Legacy vectors `:5997,6025` | U+K: preserve hooks/extender chains and reserved ROM ownership. |
| 26,55 | PSP creation in dispatch, `int21_create_child_psp:3065` | U+K: distinguish copy PSP from child PSP; transactional lifecycle notification. |
| 29 | Legacy parser `:7172`; bound VMM CP437 parser | U+K: upstream parser reconciled with common CP437 aliases/NLS. |
| 2A–2D | Date/time `:5290,5313`, legacy set branches | U: upstream, same packed date/time tests; no fake wall-clock success. |
| 2E,54 | Verify flag in dispatcher | U: upstream state. |
| 30,33 | Version/break `:5233,5402`; 3306 etc. per actual subfunction coverage | U+K: compatibility version remains numeric DOS behavior; identity text is separate. |
| 31,48–4A,58 | TSR/MCB allocator/strategy `:12079,13318,13423,13520,5954` | U+K: upstream allocator; keep lifecycle and F1 compatibility where callers remain. No memory cap change. |
| 32,34,52,5D06 | DPB/InDOS/List-of-Lists/SDA `:5477,5502,5510`, runtime mirror | K: expose actual FreeDOS structures, field-granular synchronization; no compact-SDA overwrite. |
| 37 | Switch-character branch in legacy dispatch | U: retain upstream supported subfunctions. |
| 38,65,66 | Legacy country/page `:5246,5178`; 65 only in active VMM `dos_file_nls.inc` | K: common country 1/CP437 tables, all 65 01–07/20–23 and error cases, stable pointers. |
| 39–43 | Legacy mkdir/rmdir/chdir/create/open/close/read/write/delete/seek/attrs (`:6729,7401,8161,8948,9035,9239,9596,10332,10461,10667,11118`) bypassed while bound | U+K: redirector callouts and explicit seek hook over one native owner. |
| 44 | Legacy IOCTL `:6443`; bound regular-file and mapped-drive subset, devices chain | U+K: same type split; unsupported subfunctions fail. Do not equate all handles 0–4 with CON after redirection. |
| 45,46 | Legacy duplicate branches and handle swaps; bound VMM refcounts | K: preflight/commit/rollback hooks in FreeDOS duplication and CloneHandle. |
| 47,4E,4F | Legacy cwd/find `:7126,7516,7566`; bound native search tokens | K: shared CDS, exact 43-byte DTA and copied cursor lifetime; 111B/111C adapters. |
| 4B00/01/03 | COM/MZ/overlay EXEC `:2307,3368,4267,4537`; bound internal file adapter | U+K: FreeDOS task.c loader reads via bound SFT; keep environment/FCBs, rollback and launch tracing. |
| 50,51,62 | Selected PSP `:6704,6718`; native owning ancestry distinct | K: FreeDOS fast INT21 path is also hooked; preserve immutable EXEC parent. |
| 53 | No explicit legacy selector | U: upstream DPB translation, append cases if exposed/used. |
| 56,5700/01 | Legacy rename `:10869` and file dates; active VMM | K: 1111 + explicit timestamp hook; shared description metadata and reopen. |
| 59/BX=0 | Legacy error branch and bound extended-error context | K: persistent AX/BH/BL/CH through INT24 and cleanup, no loss on callback. |
| 5A,5B,5C00/01,6C00 | Missing legacy dispatch, active f3-49 VMM implementations | U+K: upstream entrypoints feed native atomic create/locks/open actions; no successful lock stub. |
| 5D other,5E,5F | Legacy has limited 5D; network call families not claimed | U+K: upstream supported behavior only, chain unowned calls; never advertise a network stack from local IFS. |
| 60,67,68 | Legacy path/JFT/commit branches; active VMM | K: path policy/expansion/commit hooks, 232-handle admission ceiling. |
| 61,63,64,69,6A,6B,71,73,74 | Not explicit legacy services; upstream has various implemented/unsupported branches | U: inventory exact upstream subfunctions; append tests for newly exposed behavior, never claim all are complete. |
| F149/F14A | Private FAT16 memory service `:1560–1581`, retained-ancestor free/resize | K: compatibility adapter in fork if VMFORK callers use it; validate ownership and MCBs. It is not a Microsoft DOS function. |

88/89 and C2 are **not INT21 selectors here**. `floppy_stage1.asm:22192–22200`
dispatches INT15/88 and INT15/C2 mouse BIOS; `:23734–23749` dispatches XMS
function 88/89 through the XMS entry. The older summary's attribution of
these numbers to INT21 is incorrect. Preserve their real interfaces through
the VMM/virtual device owner, not new INT21 entries.

## Multiplex, kernel ABI and integration

| Interface and source | Classification/disposition | Acceptance |
| --- | --- | --- |
| DFVM discovery/bind/lifecycle, `src/runtime/ciukidos_vmm_hook.inc`, `dos_file_hook.h`, [wire note](../validation/f3-50-ciukidos-vmm-hook.md) | K+D. Preserve v1 for old guest; new checked FreeDOS layout and redirector-frame role require lead reservation. | Old/absent/malformed/spoofed gates; immutable role bytes; no overlap; rollback; exactly-once events. |
| INT2F 1203/1216/1220/1226/122D, active native bridge | K. Adapt to actual FreeDOS internals; do not re-use old hardcoded SDA/SFT offsets indiscriminately. | Direct memory inspection; internal 1226 stack preconditions; no second filesystem. |
| INT2F 1600/1605/1606/1607, `floppy_stage1.asm:23523` onward | U+K/D for any remaining Ciuki caller. Preserve documented Windows negotiation/chaining with actual source-backed capability; no fabricated Windows-host claim. | Unknown calls unchanged; existing Windows/DPMI probes retain behavior. Win16 implementation scope stays deferred. |
| 1680/1689 and keyboard idle, `:899–922,23504–23509,23618`; native `dosvm.c:703–782,1096` | U+K. Upstream INT28/HLT plus missing 1680 yield; retain VMM idle detection/focus accounting. | Busy games retain CPU, idle prompt bounded wake, no lost key/IRQ. |
| 1687 DPMI / 4300,4310 XMS and XMS 88/89, INT67 EMS | U/native host; do not port legacy embedded memory managers over the host. | 16/32-bit DPMI clients, DOS/4GW, DJGPP/go32, A20/HMA and EMS regressions. |
| CIUKIDOS header ABI v2 / RTSV 13 services, `src/runtime/ciukidos_abi.inc:4–18`, `floppy_stage1.asm:4–41` | R from unified boot. FreeDOS SYS starts at 0060 and relocates; old header loading at 0900 is incompatible. Keep old guest intact. | Dedicated profile loader checks; no counterfeit header; new selector reserved by lead. |
| RTSV identity/version/ready/default/state/caps/process-DTA/termination services, `ciukidos_kernel_services.inc:61–269` | U+K/D through DOS services/DFVM or a checked compatibility shim for live consumers. | Enumerate consumers before removal; same externally observable state and records. |
| RTSV memory-layout service and F149/F14A | K/D where a surviving VMFORK/runtime consumer needs it. | Owner-aware free/resize and accurate memory accounting; no raw native address. |
| RTSV CLFN v1 six sector/enter/leave operations, `ciukidos_lfn_abi.inc` | R from bound unified file path: direct FAT mutation conflicts with VMM owner. Preserve old disabled-binding use. Re-express needed namespace functions through documented native service. | Reachability/consumer proof; no active sector-level alias writer. |
| INT10 FF43/BX5744 query and cooperative window ABI3, `src/com/dos_window_abi.inc` | D/native VMM/runtime ownership. Kernel remains DOS API supplier, not native desktop compositor. | Query/callback/close/focus/text/graphics and input/audio capabilities remain coherent. |
| INT15 C2 mouse, INT33, VBE/INT10, PIT/PIC/keyboard | U/native virtual hardware; legacy emulation not copied into FreeDOS. | Existing device probes plus title input/video/audio. |

## Shells and builtins: do not confuse the three sources

1. The embedded recovery/text shell in `floppy_stage1.asm` is reachable in
   the old runtime; its handlers begin `:18690–18940`. It is not the current
   canonical window's standalone shell binary.
2. The canonical `SYSTEM/SHELL.COM` is `src/com/dosvm_shell.asm:1–113`, a
   wrapper which runs AUTOEXEC lines using COMMAND /C, then interactive
   COMMAND and requests VM exit through port EA when it returns.
3. `/COMMAND.COM` is `src/com/shell.asm` built with COMMAND_COMPAT=1 by
   `scripts/build_dos_image.py:238–244`. Non-COMMAND_COMPAT includes supply
   the older desktop/runtime functions and must not all be ported into DOS.

| Known command/behavior | Source evidence | Unified classification and requirement |
| --- | --- | --- |
| HELP, VER, ECHO | `shell.asm:179–229`, help data | U+D: FreeCOM implementations with CiukiDOS identity, English help and preserved credits. |
| CLS/CLEAR | `shell.asm:230–276` | U+D: preserve CLEAR alias and screen behavior. |
| CD/CHDIR, compact CD.., DIR, MD/MKDIR, RD/RMDIR | `shell.asm:459–589,704–735` | U: upstream equivalents, preserve aliases and errors via common filesystem. |
| TYPE, COPY, DEL/ERASE, REN/RENAME/MOVE | `shell.asm:590–703,736–902` | U+D: aliases; MOVE's old rename-only behavior tested separately from any future cross-volume copy/delete semantics. |
| PWD | `shell.asm:365–373`, embedded `shell_cmd_pwd:18876` | D: builtin printing current DOS path through DOS API. |
| PATH | `shell.asm:350–364`, `msg_path:3566` | U+D: initialize known directories, support genuine installer PATH changes instead of fixed display only. |
| WHERE | `shell.asm:374–431`, prefix/fallback tables `:3630–3678` | D: preserve search order/known mappings where the target exists; report absence honestly. |
| RUN, implicit external lookup, COM/EXE execution | `shell.asm:433–457`, embedded `shell_try_exec_token:18620` | U+D: preserve command tails and aliases, add FreeCOM's BAT support, COMSPEC and environment semantics. |
| EXIT/QUIT | `shell.asm:277–306`, wrapper `dosvm_shell.asm:72–82` | D: child /C exit returns status, interactive top shell closes VM cleanly, never powers down native OS. |
| REBOOT, SHUTDOWN seconds, /T, STATUS, CANCEL | `shell.asm:308–349,1130–1245`, messages `:3586–3600` | D: retain command grammar/queue; scoped existing host request or explicit error, no bare-metal reset. |
| EDIT and network names in help/WHERE | `shell_help_data.inc`, `shell.asm:3630–3678` | D: these resolve external utilities, not kernel builtins. Preserve launch mappings for shipped approved utilities; no claim this directive supplies networking. |
| Prompt/history/tab completion | embedded `:17720–18190`; standalone history storage and command input | U+D: FreeCOM equivalents, same prompt shape with CiukiDOS SHELL identity; tests for empty/backspace/history/completion/key wake. |
| Batch /C, /K, Ciuki /W, AUTOEXEC | `dosvm_shell.asm:27–74`, `shell.asm:1427–1510` | D: preserve once/keep-open/window entry behavior in FreeCOM adapter; resolve switch collisions explicitly; use real batch engine rather than line-at-a-time launcher. |
| Embedded HELP ALL, TICKS, DRIVE/DRIVES, DOS21, COMDEMO, MZDEMO, FILEIO, GFXDEMO/GFXRECT/GFXSTAR, FINDTEST, MOUSE, KEYTEST, BEEP | `floppy_stage1.asm:17230–17400`, guarded by STAGE1_DEBUG_COMMANDS | R from normal unified user shell: compile-time diagnostic commands become existing runtime-selected probes/driver diagnostics. Preserve diagnostic capability and old source, not a new production compile-time variant. |
| Embedded CDUP, RUN, DIR, PWD, CD, EXIT | `:18733–19126` | U+D: preserve user spelling/intent through FreeCOM alias/builtin; no second embedded shell owner. |
| Embedded HALT/REBOOT | `:17400` onward | R from direct hardware implementation; shell's scoped power interface as above. |
| Old embedded desktop/window compositor | non-COMMAND_COMPAT includes `shell.asm:3711–3730` | R from unified DOS kernel: native desktop/virtual VGA already own the supported window. No desktop feature is removed from CiukiOS. |

## Records, launch and regression boundaries

There is no literal `CIUKI_TEST` emitter in `floppy_stage1.asm`; it must not
be invented in the port. The existing native record machinery wraps guest
bytes as evidence (`src/kernel/vm/dosvm_bios.c:426–470`, especially :440).
Preserve the kernel-owned VM record port E9, record rate/size/drop accounting,
run/generation attribution and payload PASS/FAIL origin. Never let a guest
inject a native CIUKI_TEST envelope or infer PASS from screen text.

| Protocol | Source and port requirement |
| --- | --- |
| DOSREADY | `src/probes/doscompat/dosready.asm`, `dosvm_bios.c:360–389`: exact version/free-paragraph/drive payload from real DOS queries. Do not hardcode old kernel memory or synthesize ready. |
| RUNREC | `src/probes/dos/run_record.asm:1–15`: CHDIR/EXEC/AH4D, PASS or exact R/T/E/D/U failure; ordinary unchanged COM continues to work. |
| D48 results and shell-ready barrier | `tests/host/dos/{cases.py,shell_ready.asm}`, `scripts/dos_conformance.py`: preserve tags/bytes/probe hashes and controller teardown. |
| Launch arm, EXEC and first video/input failure | `dosvm.c:75–82,985–987,1100–1103`, `dosvm_bios.c:275–276`, `apps/dosbox/autorun.h`: kernel must retain a normal 4B boundary. Prompt alone is not launch success. |
| Guest exit/fatal ports | `dosvm_shell.asm:77–85`, VMM port decoder: preserve clean close and contained failure without host panic. |
| Kernel/base-specific overlay and digest | `dosvm.c:1434`, `dosvm_disk.c` digest cache, f3-51 amendments and f3-51c: SHA covers whole base; new content/profile never reuses old guest overlay; cache invalidates by content identity. |

## Deferred evidence, not dropped features

The full public title and private approved-library runtime inventory belongs
to f3-59. Existing static candidates are not execution traces. The contracts
must preserve DOOM's DOS/4GW, Quake's DJGPP/go32, real-mode Wolf3D, COM/MZ/
overlay and 16/32-bit DPMI paths without patched binaries. Every row above
maps to f3-55 boot/provenance, f3-56 binding/FCB/NLS, f3-57 filesystem ownership,
or f3-58 shell/launch/idle host tests; f3-59 supplies actual title evidence.
Retire no old guest before both the 100% battery and all-title gates close.
