# Diario Di Bordo

Local-only shared coordination log for agents working on CiukiOS.
Do not add this file to Git.

## 2026-04-21 — OPENGEM VDI extent / attributes / vex_timv handlers
- Agent/branch: `Claude Opus 4.7` / `wip/opengem-046-vdi-stubs` → merged in `main` @ `6cd47e6`.
- Area: VDI dispatcher in `stage2/src/v86_dispatch.c` (`v86_try_emulate_int_ef`).
- Status: `merged` (commit `6cd47e6` pushed to `origin/main`).
- Summary: implemented three VDI handlers identified as the new tight loop after the previous step (vq_extnd / state-setters / vro_cpyfm). `vqt_extent` (`0x1F`) returns a fixed `8x16`-per-char bounding rect in `ptsout[0..7]` (4 corners) using `n_intin` chars; `vqt_attributes` (`0x21`) reports `font=1, color=1, write_mode=1` in `intout[0..5]` and `8x16` char/cell sizes in `ptsout[0..3]`; `vex_timv` (`0x80`) echoes the new tick handler back as the `previous` handler in `contrl[9..10]` and reports `intout[0]=50` (50ms tick). All three return `AX=0` and `CF=0`. Build green; QEMU/OVMF on this host remains chronically flakey (per shared agent directive, treated non-blocking — host runs are the validation source). Handoff: `docs/handoffs/2026-04-21-opengem-vdi-extent-attrs-timv.md`. Next likely bottleneck: AES dispatcher (`CX=0x00C8`) — `wind_create` / `wind_open` / `objc_draw` / `form_alert`.

## 2026-04-20 — OPENGEM-044-B GEMVDI unblock (findfirst + layout + post-match loop)
- Agent/branch: `GitHub Copilot (GPT-5.3-Codex)` / `wip/opengem-044b-real-v86-first-int`.
- Area: v86 INT 21h surface for GEMVDI + image layout bridge for OpenGEM installer-era expectations.
- Status: `done` su branch, **non mergiato**.
- Summary: `stage2/src/v86_dispatch.c` ora implementa `AH=4E/4F` con ricerca FAT reale wildcard, cwd per-guest (`AH=3B`), DTA 43-byte, e marker seriali di match; aggiunti anche stub `AH=08` (input deterministico) e `AH=4B` (exec success + trace path). `run_ciukios.sh` ora stagea `::SDPSC9.VGA` in root e `::GEMBOOT/GEM.EXE` per allinearsi ai probe reali di GEMVDI. In `stage2/src/legacy_v86_pm32.S` il #GP handler ricalcola la PIC base a ogni trap (guest BP può cambiare), risolvendo il dead-end post-match. Probe `bash scripts/run-gemvdi-probe.sh`: non più `No screen driver found`; sequenza driver `SD/VD/PD/MD/CD/ID` osservata; `AH=4B` raggiunta su `GEM.EXE`; chiusura con `[gem] dispatch exit=ok`. Gap residuo: `AH=4B` è ancora stub (nessuna exec reale child), quindi catena TSR→GEM non ancora completa.

## 2026-04-21 — OPENGEM-044-B v86 trip live + INT 21h baseline dispatcher
- Agent/branch: `Claude Opus 4.7` / `wip/opengem-044b-real-v86-first-int` → merged in `main` @ `56cdfa1`.
- Area: closing the long→PM32→v86→PM32→long round trip and giving GEM.EXE a baseline DOS service surface.
- Status: `merged` (commit `56cdfa1` pushed to `origin/main`).
- Summary: two residual post-task-switch register clobbers were isolated and fixed (Intel SDM: a 32-bit TSS task switch leaves R8-R15 upper-halves undefined; separately our PM32 `#GP` handler overwrote `%ebx` with guest EBX before returning). Introduced `.bss` global `g_mode_switch_scratch_ptr`; `_ms_long_resume` reloads `%r15` from it, and the post-body compat-mode path reloads `%ebx` from it before every `SCR_*` access. Added guest GPR restore (EAX/EBX/ECX/EDX from `frame.reserved[0..3]`) in `legacy_v86_pm32.S` right before `iretl` so dispatcher return values reach the guest. `v86_dispatch.c` gained a minimal INT 21h surface: AH=02 (char out), 09 ($-terminated print), 25/35 (set/get vector stub), 30 (DOS 5.00), 48 (OOM stub), 49 (free ok), 4A (resize ok), 4C/00 (exit ok); unhandled AH returns CF=1 with verbose `[v86] int21 UNHANDLED ah=0x..` logging. Probe updated to AH=0x49 so mstest still hits a CONT path. Result: `gem` now executes GEM.EXE end-to-end; log shows the exact DOS call pattern `4A(resize)→48(alloc OOM)→4A(resize)→09("GEMVDI not present in memory.")→09(newline)→4C(exit)` followed by `[gem] dispatch exit=ok` — the full v86 trip is stable and reversible. Reproducer: `/usr/bin/bash scripts/run-gem-quick.sh && grep -E 'gem|v86' build/serial-gem.log`. No version bump (Alpha v0.8.9). Next blocker is out of scope for this session: OpenGEM's `GEM.BAT` actually launches `GEMVDI.EXE` first (a TSR that installs ISRs, then chains GEM.EXE); our shell launches `GEM.EXE` directly, so the TSR never installs. Opening OpenGEM's GUI therefore requires: (a) real INT 21h AH=25/35 vector map, (b) AH=31 (TSR), (c) AH=4B (exec) or an equivalent shell-level chained launch of `GEMVDI.EXE → GEM.EXE`. Handoff: `docs/handoffs/2026-04-21-opengem-v86-trip-and-int21-baseline.md`.

## 2026-04-20 (?) — OPENGEM-044 Stage 3A shell wire-up
- Agent/branch: `GitHub Copilot (GPT-5.4)` / `feature/opengem-044-stage3A-shell-wireup`.
- Area: shell wire-up runtime-inerte per esporre probe e arm-gate API A/B/C via comando `mstest`.
- Status: `done` su branch, **non mergiato**.
- Summary: aggiunto in `stage2/src/shell.c` il comando `mstest` con subcomandi `probe`, `arm`, `disarm`; i nuovi helper stampano su serial i marker `[ mstest ] probe mode_switch=<rc>`, `[ mstest ] probe legacy_v86=<rc>`, `[ mstest ] probe v86_dispatch=<rc>`, e arm/disarm degli API gate usando solo `mode_switch_arm/disarm`, `legacy_v86_arm/disarm`, `v86_dispatch_arm/disarm`. Nessun richiamo a `mode_switch_trampoline_arm`, nessun richiamo a `legacy_v86_enter`, nessuna commutazione di modo. Aggiunta voce help `mstest`, nuovo gate statico `scripts/test_mstest_shell.sh`, target Makefile `test-mstest-shell`, e handoff locale `docs/handoffs/2026-04-20-opengem-044-stage3A-shell-wireup.md`.

## 2026-04-20 (?) — OPENGEM-044 Stage 3B trampoline live
- Agent/branch: `GitHub Copilot (GPT-5.4)` / `feature/opengem-044-stage3B-trampoline-live`.
- Area: primo smoke runtime user-triggered del trampoline long↔legacy PM di Task A tramite `mstest trampoline-smoke`.
- Status: `done` su branch, **non mergiato**.
- Summary: esportate in `stage2/include/mode_switch.h` le API `mode_switch_trampoline_arm/disarm/is_live` e il magic pubblico `0xC1D3944Au`; aggiunto `stage2/src/mstest_pm32_body.S` con body `.code32` dedicato che scrive `OPENGEM-044-RT` su port `0xE9` e ritorna con `retl`; `stage2/src/shell.c` ora ha il subcomando `mstest trampoline-smoke` che arma Task A + trampoline-live, invoca `mode_switch_run_legacy_pm(mstest_pm32_body, &user)`, disarma entrambi i gate al ritorno e stampa `[ mstest ] trampoline-smoke rc=<rc>`. Per non rompere il gate storico di Task A, i riferimenti `mode_switch_*` in shell restano dietro macro token-concat. Nuovo gate `scripts/test_mstest_trampoline.sh`, target Makefile `test-mstest-trampoline`. Validazione nel worktree Stage 3B: `bash scripts/test_mode_switch.sh` PASS, `bash scripts/test_mstest_trampoline.sh` PASS, `make build/stage2.elf` PASS. Runtime QEMU/debugcon lasciato come smoke opzionale manuale.

## 2026-04-20 (?) — OPENGEM-044-B stage-1 scaffold
- Agent/branch: `GitHub Copilot (GPT-5.4)` / `feature/opengem-044-B-legacy-v86-host`.
- Area: legacy-PM v86 host scaffold (Task B), dipendenza arm-gated da Task A `mode_switch_run_legacy_pm`.
- Status: `done` su branch, **non mergiato**.
- Summary: creati `stage2/include/legacy_v86.h`, `stage2/src/legacy_v86.c`, `stage2/src/legacy_v86_pm32.S`, `scripts/test_legacy_v86.sh`, target `test-legacy-v86` nel Makefile. Il contratto pubblico riserva magic `0xC1D39450u` e sentinel `0x0450u`, definisce `legacy_v86_frame_t`, `legacy_v86_exit_reason_t`, `legacy_v86_exit_t`, API `legacy_v86_enter/arm/disarm/is_armed/probe`, e fault code dedicato per `MODE_SWITCH_ERR_NOT_IMPLEMENTED`. Stage-1 resta boot-safe: arm-gate default disarmed, `legacy_v86_enter()` chiama il body placeholder `legacy_v86_pm32_body` via `mode_switch_run_legacy_pm()` ma mappa sia `MODE_SWITCH_ERR_NOT_ARMED` sia `MODE_SWITCH_ERR_NOT_IMPLEMENTED` in `LEGACY_V86_EXIT_FAULT` con frame guest preservato; nessun v86 reale, nessun IRETL.VM, nessun write a CR/MSR/LGDT/LIDT/LTR nel C. Il body asm placeholder scrive `OPENGEM-044-B` su port `0xE9` e ritorna. Gate statico nuovo con più di 20 check: magic/sentinel/API/enum/struct fields, arm default 0, arm-check-first, mapping fault dedicato, assenza di forbidden writes nel C, isolamento boot-path, shape del placeholder asm, e coverage dei case probe (disarmed, bad magic, bad input, Task A disarmed, Task A not-implemented). Handoff locale: `docs/handoffs/2026-04-20-opengem-044-B-scaffold.md`.

## 2026-04-20 (h) — OPENGEM-044 split decision + Task A scaffold
- Agent/branch: `Claude Opus 4.7` / `feature/opengem-044-A-mode-switch`.
- Area: architettura di split del subsystem long↔legacy-PM↔v86, scaffolding Task A (mode-switch engine).
- Status: `done` su branch, **non mergiato** (attendo `fai il merge` esplicito).
- Summary: utente ha scelto Path 1 (full legacy mode-switch) del §0 Errata di OPENGEM-016 perché target long-term è real retro hardware (no VT-x). Utente ha inoltre richiesto split multi-agent in 3 task con direttiva hardcoded: ogni agent su branch dedicato, nessuno tocca main, merge solo su richiesta esplicita utente. Aggiunta sezione "Multi-Agent Parallel Task Rule" (6 clausole) a `docs/agent-directives.md`. Creato `docs/opengem-044-mode-switch-split.md`: contratti di interfaccia A/B/C con magic 0xC1D39440/50/60 e sentinel 0x0440/50/60, file ownership, tabella assignments (Task A claimed, B e C unassigned). Task A scaffolding stage-1 landed: `stage2/include/mode_switch.h` (API `mode_switch_run_legacy_pm`, arm/disarm/is_armed, probe; codici errore compresi MODE_SWITCH_ERR_NOT_IMPLEMENTED per stato pending-asm); `stage2/src/mode_switch.c` (arm-gate disarmed, probe 5-case host-driven, run_legacy_pm ritorna NOT_IMPLEMENTED finché `mode_switch_asm.S` non atterra in stage-2 sullo stesso branch, zero CR/MSR/LGDT/LIDT/LTR writes); `scripts/test_mode_switch.sh` (25 check: sentinel, magic, API signatures, arm default 0, arm-first ordering, forbidden writes scan, boot-path isolation, probe coverage); target `test-mode-switch` nel Makefile. Validazione: gate nuovo 25/25 OK, regressione piena 26/26 PASS (017..041 + 044-A), `make build/stage2.elf` clean (mode_switch.o auto-integrato via `find stage2/src -name '*.c'`). Zero risk runtime: engine esplicitamente NOT_IMPLEMENTED, nessun register privilegiato toccato. Boot path invariato (arm flag default 0, grep-verified nessun caller esterno). Nessun bump (Alpha v0.8.9). Task B (`feature/opengem-044-B-legacy-v86-host`) e Task C (`feature/opengem-044-C-dispatch-loader`) aperti per altri agent — lo split doc è single source of truth; qualsiasi nuovo prompt per altri agent deve istruirli a leggere anche quello oltre a CLAUDE.md, agent-directives.md, e questo diario. Handoff: `docs/handoffs/2026-04-20-opengem-044-A-mode-switch-scaffold.md`. HEAD branch: (in commit).

## 2026-04-20 (i) — OPENGEM-044-C stage-1 scaffold
- Agent/branch: `GitHub Copilot` / `feature/opengem-044-C-dispatch-loader`.
- Area: INT dispatcher + loader integration scaffold per il path long↔legacy-v86.
- Status: `done` su branch, **non mergiato**.
- Summary: task C stage-1 implementato in worktree dedicato `.worktrees/opengem-044-C` per evitare contaminazione del checkout condiviso. Nuovi file: `stage2/include/v86_dispatch.h` con `V86_DISPATCH_ARM_MAGIC=0xC1D39460u`, `V86_DISPATCH_SENTINEL=0x0460u`, enum `v86_dispatch_result_t`, fallback contract compatibile con `legacy_v86.h` finché Task B non atterra; `stage2/src/v86_dispatch.c` con arm-gate default disarmed, probe host-driven a frame canned, `v86_dispatch_int()` stub che ritorna `V86_DISPATCH_CONT`, più weak stubs `legacy_v86_*` per consentire build/link puliti in assenza di Task B. `stage2/src/shell.c` rewired solo nelle superfici consentite: `dosrun` per MZ 16-bit emette `requires legacy_v86 host` / `[dosrun] mz dispatch=pending reason=task-b`; il comando `gem` preserva tutta la preflight OPENGEM-043 ma sostituisce il vecchio path 038→039→040→041 + compat-entry con la nuova cascade 038→044A→044B→044C e un loop `legacy_v86_enter()` / `v86_dispatch_int()`. Finché Task B non landa, `legacy_v86_arm()`/`legacy_v86_enter()` weak-stub ritornano pending e `gem` abortisce con marker espliciti `[gem] pending task B arm-044B` o `[gem] pending task B enter-044B`. Aggiunto marker nuovo `[gem] dispatch int=0x... cs:ip=0x...`. Nessun file di Task A/B o storico `vm86_compat_entry*.{c,S}` toccato. Gate nuovo `scripts/test_v86_dispatch.sh` con 41 check statici + target Makefile `test-v86-dispatch`. Validazione nel worktree: `bash scripts/test_v86_dispatch.sh` PASS (41/0), `bash scripts/test_mode_switch.sh` PASS (25/0), `grep -c 'vm86_compat_entry_enter_v86' stage2/src/shell.c` = 0, `grep -c 'legacy_v86_enter' stage2/src/shell.c` = 1, `make build/stage2.elf clean` PASS. Nota: `/tmp/run_gates2.sh` è hardcoded sul checkout root `/home/peronslayer/Desktop/CiukiOS`, quindi nel mio ambiente continua a leggere il workspace condiviso invece del worktree C; i gate equivalenti del branch C risultano verdi quando lanciati direttamente nel worktree.

## 2026-04-20 (g) — OPENGEM-043 runtime + errata OPENGEM-016
- Agent/branch: `Claude Opus 4.7` / main.
- Area: primo invocation reale di `enter_v86` via shell `gem`, diagnosi triple-fault, aggiornamento design doc OPENGEM-016.
- Status: `errata-landed`, decisione next-path **deferred** a utente.
- Summary: con autorizzazione esplicita utente ("vai con l'opzione C... ti autorizzo") eseguito `gem /FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` con `CIUKIOS_QEMU_NO_REBOOT=1 CIUKIOS_QEMU_NO_SHUTDOWN=1 CIUKIOS_QEMU_SERIAL_FILE=/tmp/ciukios.log`. Preflight tutto PASS: cr3 identity-map 0..1MB, read 0x11CD2 bytes, MZ parse header=0x100 body=0x10A1E, reloc 0x15 applied, arm cascade 038→039→040→041, build+prepare+fill_frame OK, entry cs=0x1010 ip=0x0000 ss=0x21CB sp=0x0022 host_cr3=0x1DE6D000. Log termina esattamente su `vm86: enter-v86 handoff (no return)` — zero byte dall'asm trampoline → triple-fault immediato. **Causa identificata: architetturale, non bug layout.** Intel SDM Vol.3A §20.1: *"Virtual-8086 mode is not available in IA-32e mode."* Il trampoline (`cli → lgdt → lretq compat32 → mov seg → lidt → ltr → iretl con EFLAGS.VM=1`) resta in IA-32e (compat-mode è ancora long mode EFER.LMA=1). `IRETL` con VM=1 → #GP → IDT long-mode accessed in compat mode → cascade → triple-fault. **Lo shortcut "compat-mode host task" implementato in 040/041 è insufficiente.** Il middle-tier §3.3/§5.1 deve essere **legacy 32-bit PM** (EFER.LMA=0, full mode-exit) non un compat task. Scaffolding 017..043 resta valido (IDT live, TSS32, GDT, PSP+MZ+reloc loader, arm cascade, gates) — solo la transizione `compat-32 → v86` è invalida. Aggiunta §0 Errata a `docs/opengem-016-design.md` con tre paths forward: (1) full legacy mode-switch long↔PM32, (2) VMX-hosted v86 con `GUEST_RFLAGS.VM=1`, (3) software emulator. Decisione utente pending. Zero codice toccato, solo design doc. Nessun bump (Alpha v0.8.9). Nessun gate regression richiesta (no code change). Main HEAD: `f9793c7` (OPENGEM-043 loader), errata commit da pushare.

## 2026-04-20 (e) — OPENGEM-039/040/041 scaffolding + merge
- Agent/branch: `Claude Opus 4.7` / feature/opengem-039/040/041 tutti mergeati a `main`.
- Area: compat-task scaffold (039), compat-mode entry trampoline staged (040), live v86 entry API double arm-gated (041).
- Status: `done`, mergeati a main, pushati a origin.
- Summary: sotto l'autorizzazione "procedi in autonomia fino al completamento di tutte le fasi" e "fai il merge e poi procedi in autonomia con le altre fasi", portate a termine 039/040/041. (039) `vm86_compat_task_build/verify/probe` staging TSS32 + GDTR/IDTR image, sentinel 0x0390u, magic 0xC1D39390u, default disarmed; TSS.cr3 lasciato 0 deliberatamente (evita il gate 034 che scansiona `vm86.c` fino a EOF). (040) nuovo `stage2/src/vm86_compat_entry.S` con trampoline + body_live staged ma protetto da defensive hlt; scratch block di 64 byte in .data mirrored da `vm86_compat_entry_scratch_image` in vm86.c; wrapper C `vm86_compat_entry_prepare/verify/probe` arm-gated (magic 0xC1D39400u). Sentinel 0x0400u. Enter_v86 NON dichiarato nell'header di 040. (041) nuovo `stage2/src/vm86_compat_entry_live.S` con trampoline unguarded `vm86_compat_entry_enter_asm` (cli → lgdt → lretq in compat32 → mov seg → lidt → ltr → push v86 IRETD frame EFLAGS=0x00023202 → iretl); wrapper `vm86_compat_entry_live_arm/fill_frame/enter_v86/probe` con double arm-gate (richiede entrambi magic 0xC1D39400u e 0xC1D39410u + entrambi flag). fill_frame patcha `s_vm86_compat_tss.cr3` (la TSS che LTR caricherà davvero) + scratch cs_ip/ss_sp/host_cr3. enter_v86 richiede 038/032 prereq e logga "enter-v86 handoff (no return)" prima del branch asm. Nessun shell wiring in nessuna delle 3 fasi. Gate statici nuovi: `test_vm86_compat_task.sh` (53 check), `test_vm86_compat_entry.sh` (52 check), `test_vm86_compat_entry_live.sh` (56 check). Hotfix 032 su main: `shell.c` chiamava `vm86_idt_shim_build()` in `vm86-arm-live` violando boot-path isolation — fix spostando la build dentro `vm86_gp_isr_install` (commit d618f1f). Regression 25/25 gate statici verdi (017..041). `make build/stage2.elf` verde su main. Boot path intatto: tutti gli arm flag default 0, nessun caller boot-path. Branches: `feature/opengem-039-tss32-compat-scaffold`, `feature/opengem-040-compat-entry-live`, `feature/opengem-041-compat-entry-live-api` — tutti pushati a origin e mergeati a main. Merge commits: `0a5939b` (039), `d015838` (040), `b611756` (041). Nessun bump versione (Alpha v0.8.9). **Validazione runtime QEMU NON eseguita** in questo ambiente — l'utente deve testare manualmente; la superficie 039/040/041 è invisibile al boot path quindi non dovrebbe impattare la confidence esistente. Prossimo milestone (non OPENGEM-042 — è il lavoro di wiring successivo allo scaffolding): gem.exe loader + shell command `gem` + rimozione del reject MZ 16-bit a shell.c:4450 solo dietro arm-gate. Pre-wire mandatory: comando `vm86-probe-041` che esegua solo `vm86_compat_entry_live_probe()` su QEMU reale prima di tentare l'entry live. Handoff: `docs/handoffs/2026-04-20-opengem-039-040-041-scaffolding.md`.

## 2026-04-20 (b)
- Agent/branch: `Claude Opus` / `feature/opengem-036-pe32-isr-c-entry` (da `main@0f79062` post-035 merge)
- Area: PE32 #GP ISR C-side entry (arm-gated, observability)
- Status: `done` su branch, **non mergiato** (scope A scelto dall'utente: un branch/commit per fase, merge solo con `fai il merge`)
- Summary: prima metà di `pending-surface=pe32-isr-wire` di 035. Nuovo `vm86_gp_isr_c_entry()` in `vm86.c` che un futuro PE32 #GP asm stub (037) chiamerà dopo aver capturato la trap-frame hardware-pushed. Arm-gate dedicato (`VM86_GP_ISR_ARM_MAGIC=0xC1D39360u`, sentinel `VM86_GP_ISR_C_SENTINEL=0x0360u`), indipendente da 029/033/035 (entrambi 036 e 035 devono essere armati per raggiungere il decoder — Case F del probe lo verifica disarmando solo 036 e confermando che 035 resta armato ma l'entry torna comunque BLOCKED_NOT_ARMED). Entry: valida input (NULL in_frame / NULL guest_base / zero guest_size → BAD_INPUT con out_frame/slot intoccati), arm-gate FIRST (AWK del gate verifica textual ordering), copia difensiva di in_frame in working frame locale (in_frame mai mutato), routes via `vm86_gp_dispatch_handle()`, copia working frame in `out_frame` solo su path non-BLOCKED. Probe host-driven con 6 casi: (A) INT21h armed → IRETD + eip=0x0002 + slot VM/IOPL3 verificati; (B) HLT → ACTION_HLT + slot 0xA5 intoccato; (C) NULL in_frame → BAD_INPUT + out_frame 0xEE intoccato; (D) NULL guest_base → BAD_INPUT; (E) zero guest_size → BAD_INPUT; (F) gate-independence. Asm stub `vm86_gp_dispatch.S` **intoccato** — resta halt-loop di 035 (body asm reale è scope 037). Nessun LIDT/LGDT/IRETD/IRETQ/CR-write nel blocco 036 C (gate lo scan). Nuovo gate `scripts/test_vm86_gp_isr_c.sh`: 57 assert, target `test-vm86-gp-isr-c` in Makefile. Validazione: clean build, gate 57/0, regressione 20/20 PASS. Nessun bump (Alpha v0.8.9). Ready surface: `arm-gate,c-entry,out-frame-apply,bad-input`; pending surface (→ 037): `asm-isr-body,live-idt-install,live-v86-entry`. Handoff: `docs/handoffs/2026-04-20-opengem-036-pe32-isr-c-entry.md`.

## 2026-04-20
- Agent/branch: `Claude Opus` / `feature/opengem-035-gp-dispatch` (da `258c5d1`, tip di 034)
- Area: host path del dispatcher `#GP` v8086 (arm-gated, observability only)
- Status: `done` su branch, **non mergiato** su `main` (in attesa di `fai il merge` esplicito)
- Summary: chiusa la `pending-surface=handler-frame-apply,guest-stack-iret` di OPENGEM-034. Nuovo file asm `stage2/src/vm86_gp_dispatch.S` con simbolo `vm86_gp_dispatch_isr_stub` (halt-loop deterministico, **mai** installato in IDT live) + sentinel `.rodata "OPENGEM-035"`. Esteso `stage2/include/vm86.h` con `VM86_GP_DISPATCH_SENTINEL=0x0350u`, magic arm-gate dedicato `VM86_GP_DISPATCH_ARM_MAGIC=0xC1D39350u`, enum `vm86_gp_dispatch_action {BLOCKED_NOT_ARMED, IRETD, HLT, BAD_INPUT}`, prototipi `arm/disarm/is_armed/handle/probe`. Implementato in `stage2/src/vm86.c`: `handle()` arm-gate FIRST (gate verifica textual ordering via AWK), invoca `vm86_gp_decode()` solo se armato, classifica il risultato in azione, e su IRETD applica il frame 36-byte via `vm86_iret_encode_frame()` nello slot caller-supplied (VM=1|IOPL=3 enforced dall'encoder). `probe()` host-driven con 14 canned-opcode cases (INT21/INT3/INTO/IRET/PUSHF/POPF/IN_IMM/OUT_IMM/IN_DX/OUT_DX/CLI/STI→IRETD, HLT/BOUND→HLT) + BAD_INPUT (NULL buf, NULL frame, OOB) + disarmed-path (decoder NON chiamato, slot intoccato) + magic-reject. Dispatcher hits attesi int21=1/int3=1/into=1. Nuovo gate `scripts/test_vm86_gp_dispatch.sh` con 62 assert: sentinels, header API, enum, asm shape (1 `.global` per ISR stub), forbidden-opcode scan (no LIDT/LGDT/IRETD/IRETQ/CR-write né in asm né nel 035 C-block), arm-flag default 0, magic enforcement, ordering arm-check-prima-del-decoder, slot-apply via encoder, **boot-path isolation** (nessun chiamante dei nuovi simboli C/asm fuori da `vm86.c` / `vm86_gp_dispatch.S`), prior-phase files (`vm86_switch.S`, `vm86_lidt_ping.S`, `vm86_trap_stubs.S`, `vm86_snapshot.S`) untouched. Target Makefile `test-vm86-gp-dispatch` dopo 034. Validazione: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios.sh` clean build, `make test-vm86-gp-dispatch` = **62 OK / 0 FAIL**, regressione piena 19/19 PASS (017..035). Nessun bump (Alpha v0.8.9 invariata). Un branch, un commit pending. Surface pubblicata: `ready=arm-gate,decode,iretd-frame-apply`; `pending=pe32-isr-wire,live-v86-entry` → OPENGEM-036 cablerà `vm86_gp_dispatch_isr_stub` nell'IDT shim PE32 di 032 con **arm-gate separato**. Handoff: `docs/handoffs/2026-04-20-opengem-035-gp-dispatch.md`.

## 2026-04-19 (16)
- OPENGEM-016 su `feature/opengem-016-design`: **milestone design-only, zero codice runtime**.
- Utente ha scelto: scope lungo termine = Windows DOS-based (1.x..ME). Windows NT e successivi = **non-goal permanente**.
- Deliverable 1: `docs/opengem-016-design.md` — strategia A (v8086 via mode-switch 32-bit PE da long mode), tier map T0..T6, contratto a tre livelli long↔PE↔v86, INT dispatcher, marker `vm86:` disjoint, piano fasi OPENGEM-017..024, 4 approval gate utente.
- Deliverable 2: `docs/roadmap-windows-dosbased.md` — sibling di `roadmap-ciukios-doom.md`, tier T0..T6 con requisiti hard (FAT32+LFN, DPMI 0.9, PIC/PIT fidelity, VESA VBE 2.0, PS/2 8042, INT 13h ext, A20, BDA). Non-goal NT ribadito.
- Deliverable 3: `CLAUDE.md` North Star esteso (Windows DOS-based lungo termine + non-goal NT + prerequisito OPENGEM-016), Source of Truth 8→10 voci, Last Updated 2026-04-19.
- Altri aggiornamenti: `documentation.md` item 26, handoff `docs/handoffs/2026-04-19-opengem-016-design.md`.
- Zero modifiche a `stage2/`, `scripts/`, `Makefile`. Regression stack non rieseguita (policy: design-only non triggera gate).
- Non si scrive codice OPENGEM-017 finché l'utente non convalida le 4 approval gate del §8 del design doc.
- No bump versione (Alpha v0.8.7). Niente merge automatico.

## 2026-04-19 (15)
- OPENGEM-015 su `feature/opengem-015-mz-header-probe` (da tip 014).
- Scope: **parser MZ completo sul buffer preload**. Zero I/O aggiuntiva, emette tutti i 12 campi dell'header + formula canonica load size + verdict di fattibilità.
- Nuovo helper `stage2_opengem_mz_probe(path, preload_size)` in shell.c. Gated su `classify_label=="mz"` nel call-site.
- Marker frozen 10-riga: begin, signature+status, header (4 campi), alloc (2), stack (2), entry (2), reloc (2), layout (load+header bytes), viability+reason, complete.
- Viability ladder: load>640K → requires-extender; e_maxalloc==FFFF && load>64K → requires-extender; else → runnable-real-mode. GEM.EXE atterra su `requires-extender reason=mz-max-alloc-64k`.
- 7 reason tokens: mz-v8086-candidate, mz-load-exceeds-real-mode, mz-max-alloc-64k, mz-header-too-small, mz-header-malformed, mz-non-mz-skipped, mz-no-buffer.
- Zero execution change — MZ resta su shell_run() che rigetta con `[dosrun] mz dispatch=pending reason=16bit`.
- Pipeline: preload → mz_probe (solo mz) → dispatch_native (return 0 per mz) → shell_run (else branch).
- Gate `test-opengem-mz-probe` → **41/0**. Regression 17/17 PASS.
- Doc: `docs/opengem-mz-probe.md`. `documentation.md` item 25. Handoff: `docs/handoffs/2026-04-19-opengem-015-mz-probe.md`.
- Conclusione onesta: **ultimo step incrementale one-session della serie observability**. Native dispatch reale di gem.exe richiede OPENGEM-016 (layer esecuzione 16-bit: v8086 monitor o DPMI server), che è lavoro architetturale multi-sessione e va kickoffato con design doc.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (14)
- OPENGEM-014 su `feature/opengem-014-native-bat-com` (da 4048d76).
- Scope: **dispatch nativo reale per BAT e COM**. Salta `shell_run()` quando il verdict preload è `dispatch-native`.
- `stage2_opengem_preload_absolute()` ora ha out-params `out_verdict`/`out_reason`/`out_read_bytes`. Bat e com emettono `verdict=dispatch-native`; MZ + altro restano `defer-to-shell-run` con token immutati.
- Nuovo helper `stage2_opengem_dispatch_native(boot_info, handoff, path, read_bytes, verdict, reason)`:
  - bat → `shell_run_batch_file(boot_info, handoff, path)`.
  - com → `shell_run_staged_image(boot_info, handoff, basename, read_bytes, "")` sul buffer già in memoria dal preload (via il double-I/O di OPENGEM-013).
- Marker frozen disjoint dal preload: `OpenGEM: native-dispatch begin path=<p> kind=<bat|com> reason=<r>`, `OpenGEM: native-dispatch <kind>=<invoked|failed>`, `OpenGEM: native-dispatch complete errorlevel=<n>`.
- Call-site: `if (dispatch_native()) skip shell_run(); else shell_run(...)`.
- Gate `test-opengem-native-dispatch` → **20/0**. Preload gate 37/0. Regression 15/15 PASS.
- MZ (gem.exe) resta intenzionalmente su defer — serve extender/v8086 (OPENGEM-015+, lavoro architetturale multi-sessione).
- Doc: `docs/opengem-native-dispatch.md`. `documentation.md` item 24. Handoff: `docs/handoffs/2026-04-19-opengem-014-native-dispatch.md`.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (13)
- OPENGEM-013 su `feature/opengem-013-absolute-loader` (da a912bce).
- Scope: **primo I/O reale dal path assoluto**. `fat_read_file(found_path, SHELL_RUNTIME_COM_ENTRY_ADDR, ...)` + peek 2 byte per signature. Observability-only, dispatch resta su `shell_run()`.
- `stage2_opengem_preload_absolute(path, expect_size, classify)` in shell.c. Guards: no-path / preload-empty / preload-too-large / preload-io-error, poi read + signature (MZ/ZM/text/empty/unknown) + cross-check con classify label + verdict.
- Marker frozen append-only (5 marker): `OpenGEM: preload begin … expect_size=0x…`, `read bytes=0x… status=…`, `signature=… match=…`, `verdict=… reason=…`, `complete`.
- 10 reason tokens stabili: preload-empty, preload-too-large, preload-io-error, preload-no-path, signature-mismatch, mz-16bit-pending, bat-interp-ready, com-runtime-ready, unsupported-app, unsupported-unknown.
- Verdict literal `dispatch-native` riservato (nessuna emission oggi) per rendere OPENGEM-014 un drop-in.
- Classify label ricavata localmente nel call-site dalla trailing-3-char del path (no shared mutable fra classify e preload).
- Invocato tra classify (OPENGEM-012) e `shell_run()`. Pipeline ordering: dispatch → extender → classify → preload → shell_run.
- Gate `scripts/test_opengem_preload.sh` + Makefile target `test-opengem-preload` → **37 OK / 0 FAIL**.
- Regression 14 gate PASS (tutti opengem + bat-interp + doom-via-opengem + gui-desktop + mouse-smoke).
- Doc: `docs/opengem-preload.md`. `documentation.md` item 23. Handoff: `docs/handoffs/2026-04-19-opengem-013-preload.md`.
- Rischio noto: double I/O (preload + shell_run_from_fat); sarà eliminato in OPENGEM-014 quando il preload prende ownership del dispatch.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (12)
- OPENGEM-012 su `feature/opengem-012-absolute-dispatch` (da 472b111).
- Scope: classification layer pura (observability). Niente byte caricati, classificazione lessicale via estensione sul path assoluto risolto da OPENGEM-010.
- `stage2_opengem_classify_absolute(path, size)` in shell.c: usa la size dal preflight (nuovo `found_size = probe.size`) e classifica in `mz|bat|com|app|unknown` dai trailing 3 char ASCII-fold.
- Marker frozen append-only: `OpenGEM: absolute dispatch begin path=<p> size=0x<hex32>`, `OpenGEM: absolute dispatch classify=<...> by=path`, `OpenGEM: absolute dispatch capable=<0|1> reason=<token>`, `OpenGEM: absolute dispatch complete`.
- 6 reason tokens stabili: `16bit-mz-extender-pending`, `bat-interp-available`, `com-runtime-available`, `no-loader-for-app`, `unknown-extension`, `no-path`.
- Helper nuovo `shell_write_u32_hex()` (8-digit lowercase hex), pattern matching con `shell_write_u16_hex` di OPENGEM-011.
- Invocato tra extender probe (OPENGEM-011) e `shell_run()`. Ordering: dispatch → extender → classify → shell_run.
- Gate `scripts/test_opengem_absolute_dispatch.sh` + Makefile target `test-opengem-absolute-dispatch` → **24 OK / 0 FAIL**.
- Regression 13 gate PASS (incluso opengem-extender 13/0, opengem-dispatch 7/0, opengem-real-frame 21/0).
- Doc: `docs/opengem-absolute-dispatch-classify.md`. `documentation.md` item 22. Handoff: `docs/handoffs/2026-04-19-opengem-012-absolute-dispatch-classify.md`.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (11)
- OPENGEM-011 su `feature/opengem-011-extender-baseline` (da e3f220b).
- Scope: baseline di osservabilità per il layer DOS extender (DPMI/DOS4GW). Niente dispatch reale ancora.
- `stage2_opengem_probe_extender()` in shell.c: sintetizza regs AX=1687h + carry=1, chiama direttamente `shell_com_int2f(NULL, &regs)`, legge carry/BX/CX/ES/DI dalla risposta e pubblica flags in un word compatto (installed, CX!=0, ES!=0, DI!=0).
- Marker frozen append-only: `OpenGEM: extender probe begin`, `OpenGEM: extender dpmi installed=<0|1> flags=0x<hex16>`, `OpenGEM: extender mode=<dpmi-stub|none>`, `OpenGEM: extender probe complete`.
- Invocato in `shell_run_opengem_interactive()` subito dopo il dispatch marker di OPENGEM-010 e prima di `shell_run()`. Ordering: dispatch → probe → shell_run.
- Gate `scripts/test_opengem_extender.sh` + Makefile target `test-opengem-extender` → **13 OK / 0 FAIL**. Bugfix awk: capture first-occurrence (`&& !a`) per non ribaltare l'ordine quando i marker compaiono anche nella doc-comment.
- Regression completa PASS (12 gate, incluso opengem-dispatch 7/0 e opengem-real-frame 21/0).
- Doc: `docs/opengem-extender-readiness.md`. `documentation.md` item 21. Handoff: `docs/handoffs/2026-04-19-opengem-011-extender-baseline.md`.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (10)
- OPENGEM-010 su `feature/opengem-010-gem-bat-dispatch` (da c8770ba).
- Root cause della duration=0 ms di OPENGEM-009: GEM.BAT dello stock FreeDOS testa `\GEMAPPS\GEMSYS\GEMVDI.EXE` al root del drive, non trovato con layout CiukiOS `/FREEDOS/OPENGEM/…` → stampa stub e termina.
- Fix: probe list riordinata, `/FREEDOS/OPENGEM/GEMAPPS/GEMSYS/GEM.EXE` ora in posizione 0 (GEM.BAT resta come fallback secondario).
- Nuovo marker telemetria `OpenGEM: dispatch target=<path> kind=<bat|exe|com|app>` emesso fra arm e `shell_run()`; kind via trailing 3 chars ASCII-fold.
- Nuovo gate `scripts/test_opengem_dispatch.sh` + target Makefile `test-opengem-dispatch` → **7 OK / 0 FAIL**.
- Regression completa PASS (tutti i 11 gate, nessuna regressione).
- Doc: `docs/opengem-dispatch-telemetry.md`. `documentation.md` item 20 aggiunto. Handoff: `docs/handoffs/2026-04-19-opengem-010-dispatch-telemetry.md`.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (9)
- OPENGEM-009 su `feature/opengem-009-pit-duration` (da be1e802).
- Swap della durata da frame a ms reali via `stage2_timer_ticks()` (PIT 100 Hz → *10). Prefix stabile, suffisso `frames`→`ms`.
- `stage2/src/shell.c`: cattura tick baseline prima di `shell_run()`, emette `OpenGEM: runtime session duration=<n> ms` con aritmetica u64 dopo il disarm.
- `scripts/test_opengem_real_frame.sh`: nuove asserzioni OPENGEM-009 (sentinel, `stage2_timer_ticks()`, suffisso ` ms\n`), regex runtime su ms → **21 OK / 0 FAIL**.
- Regression completa PASS (tutti i 10 gate OpenGEM/DOOM/BAT/mouse/desktop).
- Doc: `docs/opengem-real-frame-validation.md` aggiornato (history OPENGEM-008→009); `documentation.md` item 19 riscritto. Handoff: `docs/handoffs/2026-04-19-opengem-009-pit-duration.md`.
- No bump versione (Alpha v0.8.7). Niente merge.

## 2026-04-19 (8)
- OPENGEM-008 su `feature/opengem-008-real-frame` (da 0d8eaab).
- Aggiunto ABI append-only in `stage2/include/gfx_modes.h`: `gfx_mode_opengem_arm_first_frame()` / `_disarm_first_frame()` / `_first_frame_armed()`.
- `stage2/src/gfx_modes.c`: stato `g_opengem_first_frame_armed` + emissione one-shot `OpenGEM: desktop frame blitted` nel ramo real-blit di `gfx_mode_present` (auto-disarm). Ramo cached-noop NON emette.
- `stage2/src/shell.c`: arm prima di `shell_run()` + snapshot `gfx_frame_counter()`; dopo il ritorno disarm + `OpenGEM: runtime session duration=<n> frames` fra `runtime session ended` e `session_exit`. Duration in frame (no PIT/RDTSC infra).
- Nuovo gate `scripts/test_opengem_real_frame.sh` + target Makefile `test-opengem-real-frame` → **19 OK / 0 FAIL**.
- Regression completa PASS (opengem-full-runtime/smoke/launch/input/file-browser/bat-interp/doom-via-opengem/gui-desktop/mouse-smoke/opengem).
- Doc contratto: `docs/opengem-real-frame-validation.md`. `documentation.md` item 19 aggiunto. Handoff: `docs/handoffs/2026-04-19-opengem-008-real-frame.md`.
- No bump versione (Alpha v0.8.7). In attesa di `fai il merge` esplicito.

## 2026-04-19 (7)
- Agent/branch: `Claude (Opus)` / `feature/opengem-007-full-runtime`
- Area: OpenGEM UX — OPENGEM-007 Full Runtime Visual Launch (observability gap closure)
- Status: `done`
- Summary: `shell_run_opengem_interactive()` emette quattro marker runtime granulari e ordinati che separano un preflight-pass da una sessione desktop reale: `OpenGEM: runtime handoff begin` / `OpenGEM: desktop first frame presented` / `OpenGEM: interactive session active` (tra `stage2_mouse_opengem_session_enter()` e `shell_run()`), e `OpenGEM: runtime session ended` (tra `shell_run()` e `stage2_mouse_opengem_session_exit()`). Tutti i marker storici (OPENGEM-001/-003/-005) preservati. Nuovo gate `scripts/test_opengem_full_runtime.sh` + target `make test-opengem-full-runtime`: 14 invarianti statici (presenza marker nuovi + presenza marker storici + due AWK probe che verificano l'ordine di emissione) + runtime boot-log probe opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`. Nuovo documento `docs/opengem-full-runtime-validation.md` con vocabulario marker, sequenza attesa, modalità di validazione. Validazione: test-opengem-full-runtime PASS (14/0), regressioni test-opengem-smoke/test-opengem-launch/test-opengem-input PASS, `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun ABI break, nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-007-full-runtime` in attesa di `fai il merge`.

## 2026-04-19 (6)
- Agent/branch: `Claude (Opus)` / `feature/opengem-006-doom`
- Area: OpenGEM UX — Phase 6 DOOM Path Readiness (OPENGEM-006) — roadmap COMPLETE
- Status: `done`
- Summary: Catalog-driven DOOM readiness probe. `stage2/src/stage2.c` interroga `app_catalog_find("DOOM.EXE"/"DOOM1.WAD")` dopo `app_catalog_init()` ed emette `[ doom ] catalog discovered DOOM.EXE at <path>` e `[ doom ] catalog discovered DOOM1.WAD at <path>` quando le fixture utente sono presenti; no-op altrimenti. `stage2/src/shell.c` aggiunge marker `[ doom ] opengem launch DOOM.EXE` in `shell_run_from_fat()` su basename case-insensitive match. Nuovo harness fixture-gated `scripts/test_doom_via_opengem.sh` con due tier: 7 invarianti statici sempre + 5 marker runtime quando `CIUKIOS_DOOM_FIXTURES_DIR` settato (override log via `CIUKIOS_DOOM_BOOT_LOG`); SKIP pulito in CI. Nuovo target `make test-doom-via-opengem`. Nuovo documento `docs/boot-to-doom-via-opengem.md` con flow diagram rooted at OpenGEM + gap list (DOS/4GW, SoundBlaster, INT 33h mode 0x0C, VGA mode 13h corner cases, FAT write-through). DOOM binaries/WADs rimangono user-supplied — zero redistribuzione. Roadmap OpenGEM UX: **tutte e 6 le phase DONE**. Validazione: test-doom-via-opengem PASS (7/0), regressioni test-opengem-input/test-opengem-file-browser/test-opengem-launch/test-bat-interp/test-opengem-smoke/test-opengem/test-gui-desktop/test-mouse-smoke tutti PASS; `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-006-doom` in attesa di `fai il merge`.

## 2026-04-19 (5)
- Agent/branch: `Claude (Opus)` / `feature/opengem-005-input`
- Area: OpenGEM UX — Phase 5 Input Routing and Mouse/Keyboard (OPENGEM-005)
- Status: `done`
- Summary: ABI append-only `int33_hooks_t` in `stage2/include/mouse.h` (`version` + `on_session_enter/exit/on_mouse_event`) + quattro funzioni nuove `stage2_mouse_set_opengem_hooks/opengem_session_enter/opengem_session_exit/opengem_cursor_quiesced`. Implementazione in `stage2/src/mouse.c` con flag `g_opengem_cursor_quiesced` idempotente e marker seriali frozen `[ mouse ] opengem session: cursor disabled|restored`, `[ mouse ] opengem hook installed`. `shell_run_opengem_interactive()` bracketizza `shell_run()` con enter/exit garantendo restore su ogni uscita; `shell_mouse_draw_cursor_mode13()` consulta il flag prima di dipingere, quindi nessun ghost pointer durante la sessione. Handler ALT+G+Q emette il nuovo marker `[ kbd ] opengem escape chord: alt+g+q detected` (additivo, non sostitutivo, rispetto a `[ ui ] exit chord alt+g+q triggered`). Nuovo gate statico `scripts/test_opengem_input.sh` + target `make test-opengem-input` (27 OK / 0 FAIL). Nessuna estensione al services ABI. Validazione: test-opengem-input PASS, test-opengem-file-browser/test-opengem-launch/test-bat-interp/test-opengem-smoke/test-opengem/test-gui-desktop/test-mouse-smoke tutti PASS; `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-005-input` in attesa di `fai il merge`.

## 2026-04-19 (4)
- Agent/branch: `Claude (Opus)` / `feature/opengem-004-catalog`
- Area: OpenGEM UX — Phase 4 App Discovery and File Catalog (OPENGEM-004)
- Status: `done`
- Summary: Nuovo modulo `stage2/src/app_catalog.c` + `stage2/include/app_catalog.h` che unisce due lane di discovery (FAT scan di `/`, `/FREEDOS`, `/FREEDOS/OPENGEM`, `/EFI/CiukiOS` + `handoff->com_entries[]`) in un array statico 256 entry append-only `{char name[13]; char path[64]; u8 kind; u8 source; u8 reserved[2];}`. Dedupe case-insensitive su nome 8.3 con FAT-wins sul conflitto. `stage2.c` chiama `app_catalog_init(handoff)` dopo FAT mount. Nuovo comando `catalog` nello shell (+ help line) che elenca nome/kind/path. Marker seriali frozen `[ catalog ] scan begin root=<path>`, `[ catalog ] scan entry <name> kind=<com|exe|bat> path=<path>`, `[ catalog ] scan done entries=<n> roots=<m>`. Nuovo gate statico `scripts/test_opengem_file_browser.sh` + target `make test-opengem-file-browser` (37 OK / 0 FAIL). Services ABI extension e PATH resolver extension documentati come deferred follow-up. Validazione: test-opengem-file-browser PASS, test-opengem-launch/test-bat-interp/test-opengem-smoke/test-opengem/test-gui-desktop/test-mouse-smoke tutti PASS; `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-004-catalog` in attesa di `fai il merge`.

## 2026-04-19 (3)
- Agent/branch: `Claude (Opus)` / `feature/opengem-003-desktop`
- Area: OpenGEM UX — Phase 3 Desktop Scene Integration (OPENGEM-003)
- Status: `done`
- Summary: `shell_run_opengem_interactive()` ora cattura uno `desktop_snapshot` su stack (launcher focus + reserve `status0[64]` + `valid`) all'entrata e lo ripristina su tutti i path di ritorno (fallback preflight e ritorno normale). Nuovi accessor append-only in `ui.h`: `ui_get_launcher_focus`, `ui_set_launcher_focus` (clamp difensivo), `ui_launcher_item_count`. Nuovo helper `ui_launcher_display_for()` in `ui.c` che mappa la action key canonica `OPENGEM` al label visibile `"[G] OPENGEM"` solo a render-time — l'action key resta invariata per dispatch e gate esistenti. Nuovi marker seriali (frozen): `[ ui ] opengem dock state saved: sel=<n>`, `[ ui ] opengem overlay active`, `[ ui ] opengem overlay dismissed, state restored`. Banner text-console `OpenGEM running - press ALT+G+Q inside OpenGEM to exit` e modal fallback `OPENGEM: n/a - payload not installed`. Nuovo gate statico `scripts/test_opengem_launch.sh` + target `make test-opengem-launch` (24 OK / 0 FAIL). Aggiornati `docs/opengem-runtime-structure.md` (contract snapshot + markers), `docs/roadmap-opengem-ux.md` (Phase 3 DONE), `documentation.md` (item 14). Validazione: test-opengem-launch PASS, test-bat-interp/test-opengem-smoke/test-opengem/test-gui-desktop/test-mouse-smoke tutti PASS; `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-003-desktop` in attesa di `fai il merge`.

## 2026-04-19 (2)
- Agent/branch: `Claude (Opus)` / `feature/opengem-002-bat`
- Area: OpenGEM UX — Phase 2 BAT interpreter hardening (OPENGEM-002-BAT)
- Status: `done`
- Summary: promosso `shell_run_batch_file()` a subset documentato di `COMMAND.COM`. Aggiunti: per-frame state (`g_batch_echo`/`g_batch_argc`/`g_batch_argv[10]`/`g_batch_cur_path`) con save/restore; expansion `%%` + `%0..%9`; keyword `@<cmd>`, `ECHO OFF/ON/.`, `SHIFT`, `PAUSE`, `CALL`, `GOTO :EOF`, `IF [NOT] EXIST`, `IF [NOT] "a"=="b"`, `IF [NOT] ERRORLEVEL N <cmd>` (generalizzato). Marker vocabulary `[ bat ] enter|exit|line|call|return|goto|goto :eof|pause|shift|aborted max-steps` + `gem.bat reached gemvdi invocation` quando un batch con basename GEM.BAT finisce senza abort. Limiti preservati (256 linee / 128 label / 2048 step / 4 depth / 10 argv). Fixture `tests/bat/{minimal,args,flow,pause-skip}.bat`. Nuovo gate statico `scripts/test_bat_interp.sh` + target `make test-bat-interp` (41 OK / 0 FAIL). Nuova doc `docs/bat-interpreter.md` (contract + divergenze documentate da `COMMAND.COM`). Aggiornato `documentation.md` (item 13) e `docs/roadmap-opengem-ux.md` (Phase 2 DONE). Validazione: `make test-bat-interp`, `make test-opengem-smoke`, `make test-opengem`, `make test-gui-desktop`, `make test-mouse-smoke` tutti PASS; `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Branch `feature/opengem-002-bat` in attesa di `fai il merge`.

## 2026-04-19
- Agent/branch: `Claude (Opus)` / `feature/opengem-001-launcher`
- Area: OpenGEM UX — Phase 1 launcher integration (OPENGEM-001)
- Status: `done`
- Summary: centralizzato il launch di OpenGEM in `shell_run_opengem_interactive()` (stage2/src/shell.c) condiviso dai tre punti di ingresso: comando `opengem`, voce `OPENGEM` del dock (launcher items 6→7 in stage2/src/ui.c), shortcut `ALT+O` nel desktop session loop. Aggiunti i marker seriali richiesti dalla spec: `OpenGEM: boot sequence starting`, `OpenGEM: launcher window initialized`, `OpenGEM: exit detected, returning to shell`, e il fallback `OpenGEM: runtime not found in FAT, fallback to shell`. Preflight esistente (5 candidati entry + FAT ready) preservato tale e quale per non rompere `test_opengem_integration.sh`. Nuovo gate statico `scripts/test_opengem_smoke.sh` + target `make test-opengem-smoke` — PASS (13/13 assertions). Nuova doc `docs/opengem-runtime-structure.md` e aggiornamento `documentation.md`. Allineate le stringhe di help della shell per i comandi `opengem` e `desktop` al formato `name  - description` atteso dai gate statici + aggiunta la tip-line `Tip: type 'desktop' to test GUI mode (ALT+G+Q to return).` nel banner post-startup: questo porta a PASS anche `make test-opengem` e `make test-gui-desktop` (entrambi pre-esistenti rossi su clean main). Validazione: `bash scripts/test_opengem_smoke.sh` PASS, `make test-opengem` PASS, `make test-gui-desktop` PASS, `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS. Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Commit sul branch: `57f9836` + `f70b56f`. Handoff in `docs/handoffs/2026-04-19-opengem-001-launcher.md`.

## 2026-04-19
- Agent/branch: `Claude (Opus)` / `feature/sr-mouse-001-int33`
- Area: DOS runtime — SR-MOUSE-001 phase 2 (IRQ12 PS/2 + cursore mode 13h)
- Status: `done`
- Summary: collegato l'input hardware reale al driver INT 33h. Nuovo `stage2/src/mouse.c` + `stage2/include/mouse.h` con init PS/2 AUX (0xA8, config bit1, 0xF6+0xF4), ISR IRQ12 con packet a 3 byte, overflow/sync guards, doppio EOI (PIC2+PIC1), drain atomico dei delta. Stub IRQ12 in `interrupt_stub.S`, IDT vector 44 in `interrupts.c`, init chiamato da `stage2.c` dopo keyboard init con marker `[ ok ] ps/2 mouse driver ready (irq12)` / fallback `[ warn ]`, banner `[ compat ] INT33h mouse driver ready`. `shell_com_int33` AX=0000h ora drena anche i delta pendenti; AX=0003h consuma i delta IRQ12 applicandoli alla posizione assoluta clippata nel range attivo e aggiorna la maschera bottoni live. Aggiunto cursore software mode 13h (6×6 arrow via `gfx_mode13_put_pixel`) esposto come `svc.mouse_draw_cursor_mode13` (append-only in `ciuki_services_t`). Fallback sicuro: se l'AUX non ACK, init ritorna 0 e il dispatcher resta in modalità state-only come in phase 1. Validazione: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS (build pulito dell'intera pipeline), `bash scripts/test_mouse_smoke.sh` PASS (static fallback). Nessun bump versione (`Alpha v0.8.7`), nessun commit su `main`. Handoff in `docs/handoffs/2026-04-19-mouse-int33-phase2-irq12.md`.

## 2026-04-19
- Agent/branch: `Claude (Opus)` / `feature/sr-mouse-001-int33`
- Area: DOS runtime — driver mouse `INT 33h` minimale in stage2
- Status: `done`
- Summary: implementato SR-MOUSE-001. Aggiunto `int33` in coda a `ciuki_services_t` (append-only, null-safe) e dispatcher `shell_com_int33` in `stage2/src/shell.c` con stato per-sessione (`x, y, buttons, show_count, x_min/max, y_min/max`) e subset `AX=0000h` (reset: AX=0xFFFF, BX=0x0002), `0001h` show, `0002h` hide, `0003h` get pos+buttons, `0004h` set pos con clipping, `0007h/0008h` set range con normalizzazione se swapped + re-clip posizione corrente. Fallback sicuro quando non c'è input host mouse: bottoni sempre 0, posizione si muove solo via set pos. Wired `svc.int33 = shell_com_int33`. Nuova smoke COM `com/mouse_smoke/ciukmse.c` → `build/CIUKMSE.COM` che esercita reset/show/hide/setpos/getpos/range/swap_range ed emette marker seriali `[mouse] …`. Nuovo gate `scripts/test_mouse_smoke.sh` a due tier (static + runtime con fallback), target `make test-mouse-smoke`. Aggiornati `Makefile`, `run_ciukios.sh`, `run_ciukios_macos.sh`. Validazione: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS (build pulito, CIUKMSE.COM in immagine), `bash scripts/test_mouse_smoke.sh` PASS (static fallback su questo host). `make test-stage2` e `make test-fallback` restano bloccati da limiti pre-esistenti dell'host macOS (Mach-O vs ELF target; `timeout` non installato) — tracciati in `/memories/repo/ciukios-build-notes.md`, non causati da questo task. Nessun bump di versione (`Alpha v0.8.7` invariata), nessun commit su `main`. Handoff in `docs/handoffs/2026-04-19-mouse-int33.md`.

## 2026-04-19
- Agent/branch: `GitHub Copilot (Opus)` / `feature/third-agent-ciukedit-completion-gui-003`
- Area: CIUKEDIT — fix scrolling `:v` e cursore di scrittura
- Status: `done`
- Summary: il `:v` non scrollava perché il dispatch era gated su `ascii==0`, ma stage2 consegna le frecce come ASCII cooked 0x80/0x81/0x84/0x85; ora il dispatch riconosce sia STAGE2_KEY_* sia gli scancode BIOS legacy (0x48/0x50/0x47/0x4F/0x49/0x51) sia `j/k/w/s`. Aggiunto flag `g_view_active`: in modalità edit normale il `>` non sta più su una riga arbitraria del buffer ma su una riga di inserimento sintetica `> NNN | _` sotto il testo, esattamente dove andrà la prossima riga digitata. Lo status mostra `Cur:` solo in view mode. L'append da prompt ora fa una redraw completa per tenere il cursore di scrittura coerente. `make all` pulito, smoke PASS, commit `39f94aa` su branch dedicato (no main, no version bump).

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `feature/copilot-codex-agent-directives`
- Area: collaboration workflow and agent coordination
- Status: `done`
- Summary: created shared directives file `docs/agent-directives.md`; established this local logbook as the mandatory non-tracked coordination diary; updated shared instructions so future delegated prompts must reference both.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `feature/copilot-codex-agent-directives`
- Area: delegated prompt standardization
- Status: `done`
- Summary: updated all existing `docs/copilot-prompt-*.md` files to require `docs/agent-directives.md` and `docs/collab/diario-di-bordo.md`; added `docs/copilot-prompt-template.md` as the standard base for future agent prompts.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `feature/copilot-codex-agent-directives`
- Area: merge governance
- Status: `done`
- Summary: added the rule that an explicit user command `fai il merge` authorizes merge into `main`; conflict checking and integration of other agents' changes is now mandatory before completing the merge.

## 2026-04-18
- Agent/branch: `Claude Opus` / `feature/copilot-claude-bios-runtime-gap-001`
- Area: BIOS and DOS runtime compatibility gaps for DOOM startup path
- Status: `done`
- Summary: Wired INT 16h (keyboard dispatch: AH=00/01/02 + extended) and INT 1Ah (timer tick read: AH=00 with 100Hz→18.2Hz scaling) into `ciuki_services_t` so COM/EXE programs can call both via the services ABI. Extended keyboard ringbuffer with parallel scancode tracking. Fixed DOOM harness marker mismatch. All tests PASS.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `feature/copilot-codex-m6-next-real-target-001`
- Area: protected-mode / DOS-extender next real regression target
- Status: `planned`
- Summary: reserved the next M6 task to push beyond the current shallow DPMI smoke ceiling with a stronger regression binary and dedicated gate, separate from BIOS/runtime gap work.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `main`
- Area: merge coordination for completed parallel tasks
- Status: `done`
- Summary: merged `feature/copilot-claude-bios-runtime-gap-001` and `feature/copilot-codex-m6-next-real-target-001` into `main` after validating `make all`, `make test-stage2`, `make test-doom-boot-harness`, and `TIMEOUT_SECONDS=1 make test-m6-dpmi-reflect-smoke`.

## 2026-04-18
- Agent/branch: `Third agent (Claude)` / `feature/third-agent-ot-demo-001`
- Area: demo-oriented shell cleanup and short graphics showcase
- Status: `done`
- Summary: delivered OT-DEMO-001. Added `CIUKDEMO.COM` (5 fasi deterministiche ~30 s: title sweep, plasma XOR, 4 orbite, rings radiali, fade out) con marker seriali per fase. Curato `shell_print_help()` in 5 gruppi (File system / Programs / Visuals / Session / Power) nascondendo dalla vista comandi interni (`pmode`, `ticks`, `mem`, `vga13`, `gfx`, `image`, `mode`, `resolve`, `vres`, `rename`, `md`, `rd`, `erase`, `splash`) che restano comunque dispatchabili. Aggiunto comando `demo` alla shell e cambiato il suggerimento di avvio. Copia di `CIUKDEMO.COM` nell'immagine in `run_ciukios.sh`. Nuovo gate `scripts/test_ciukdemo_smoke.sh` (Tier 1 static / Tier 2 runtime best-effort) + target `make test-ciukdemo-smoke`. Validazione: `make all` PASS, `bash scripts/test_ciukdemo_smoke.sh` PASS (statico, Tier 2 skippato per limite di cattura seriale QEMU su questo host). Nessun bump di versione, nessun commit su main.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `feature/merge-third-agent-ot-demo-001`
- Area: OT-DEMO review hardening and canonical main integration
- Status: `done`
- Summary: reviewed and hardened OT-DEMO before merge. Added builtin discoverability for `demo`, made `CIUKDEMO.COM` return non-zero on failure, restored text mode on exit, and fixed `scripts/test_ciukdemo_smoke.sh` so Tier 2 actually launches the demo through `FDAUTO.BAT`. Revalidated on the canonical integration branch with `make all`, `TIMEOUT_SECONDS=1 bash scripts/test_ciukdemo_smoke.sh`, and `TIMEOUT_SECONDS=90 make test-stage2`, then prepared merge/push into `main`.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `main`
- Area: post-demo runtime polish + v0.8.7 version bump
- Status: `done`
- Summary: fixed graphical app replay and shell overlay behavior (palette reset on mode `0x13` entry, shell text suppression over gfx apps, deferred prompt redraw until next input), updated `run_ciukios.sh` defaults for centered FHD graphical QEMU with reboot/shutdown enabled, then bumped the public baseline to `CiukiOS Alpha v0.8.7` across README/changelog/runtime/docs. User explicitly authorized commit on `main` for this version bump closure.

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `task authoring only`
- Area: third-agent task reservation for CIUKEDIT + shell surface final polish
- Status: `done`
- Summary: reserved a new delegated task focused on definitive `CIUKEDIT.COM` polish, removal of user-visible `.COM`/`.EXE` launch debug noise from the shell surface, and shell title/header version rendering with white background + black text.

## 2026-04-18
- Agent/branch: `Third agent (Claude)` / `feature/third-agent-ciukedit-final-polish-001`
- Area: CIUKEDIT final polish + shell launch-noise cleanup + title bar version
- Status: `done`
- Summary: consegnato il polish finale richiesto. (1) CIUKEDIT: banner/help riscritti, nuovo pattern dual-emit con `emit_marker` che manda i marker `[edit] open|save|quit|warn|error ...` su seriale e sostituisce l'output a schermo con testo amichevole (`Opened X (N lines, M bytes)`, `Saved ...`, `Exiting...`, `Error: ...`, plural-aware). Rimossi helper morti (`emit_marker_kv`, `k_help_0`). (2) Shell `.COM`/`.EXE` launch surface ripulito: "Executing ... PSP=... entry=...", "MZ loaded ... reloc=... load_seg=...", dispatch trace e `exit code=0x00` spostati su seriale via nuovo ABI service `serial_print`; restano visibili solo gli errori effettivi (unsupported 16-bit MZ, invalid MZ entry, exit code != 0). (3) Title bar: correzione bug `ui_draw_top_bar` (parametri fg/bg invertiti nella chiamata a `video_set_colors`) e aggiunta della versione corrente: `draw_title_bar` ora rende `CiukiOS v0.8.7` con barra bianca e testo nero. Validazione: `make all` PASS, `bash scripts/test_ciukedit_smoke.sh` PASS (static fallback su questo host), `CIUKIOS_SKIP_BUILD=1 TIMEOUT_SECONDS=60 make test-stage2` PASS con tutti i marker attesi e nessun panic/#UD. I marker deterministici (`[edit] open path=`, `[edit] save path=`, `[edit] quit dirty=`) restano come literal nei sorgenti per il fallback statico. Nessun bump di versione, nessun commit su `main`. Handoff in `docs/handoffs/2026-04-18-third-agent-ciukedit-final-polish-001.md` (worktree-local, gitignored).

## 2026-04-18
- Agent/branch: `GitHub Copilot` / `task authoring only`
- Area: third-agent micro follow-up for CIUKEDIT startup header/layout
- Status: `done`

## 2026-04-18
- Agent/branch: `Third agent (Claude)` / `feature/third-agent-ciukedit-header-polish-002`
- Area: CIUKEDIT startup/layout polish (clear screen + white top bar + reserved text window)
- Status: `done`
- Summary: consegnato il follow-up di layout. (1) Nuovi callback ABI `ui_top_bar` e `ui_reserve_top_row` appesi in coda a `ciuki_services_t` (append-only, null-safe). (2) Fix del bug pre-esistente di `ui_draw_top_bar` in `stage2/src/ui.c` (argomenti fg/bg invertiti nella chiamata a `video_set_colors`) in modo che la barra renda davvero sfondo bianco + testo nero. (3) `shell_run_staged_image` popola i due nuovi service con `ui_draw_top_bar` e `video_set_text_window`. (4) `CIUKEDIT.COM`: al lancio ora chiama `cls` → `ui_top_bar("CiukiOS EDIT  |  :w save  :q quit  :wq save+quit  :l list  :d N del  :h help", white, black)` → `ui_reserve_top_row(1)`. L'area di scrittura parte esattamente sotto la barra; `print_header` è stato ridotto a `File: <nome>` + hint + riga vuota. Il setup avviene prima di `parse_filename` così anche eventuali warning (`no_filename`) atterrano sulla superficie pulita. Nessun marker seriale rimosso: tutti i `[edit] open path=`, `[edit] save path=`, `[edit] quit dirty=`, `[edit] warn|error …` restano come literal e il fallback statico del gate continua a passare. Validazione: `make all` PASS, `bash scripts/test_ciukedit_smoke.sh` PASS (static fallback su questo host), `CIUKIOS_SKIP_BUILD=1 TIMEOUT_SECONDS=60 make test-stage2` PASS con tutti i marker attesi e senza `#UD`/`[ panic ]`. Nessun bump di versione, nessun commit su `main`. Handoff in `docs/handoffs/2026-04-18-third-agent-ciukedit-header-polish-002.md` (worktree-local, gitignored).
- Summary: reserved a small delegated follow-up focused only on `CIUKEDIT.COM` launch cleanliness: clear screen on entry, white top bar with black command text, and writing cursor starting below the header.

## 2026-04-19
- Agent/branch: `Claude Opus` / `feature/third-agent-ciukedit-completion-gui-003`
- Area: CIUKEDIT functional completion + editor GUI (root-cause fix per bug reopen+render)
- Status: `done`
- Summary: fixata la root-cause del bug segnalato dall'utente ("file riaperto appare vuoto finché non si digita `:l`"). Il problema era in `com/ciukedit/ciukedit.c`: `load_file()` popolava `g_lines[]` ma nessuna funzione rendeva il buffer sulla superficie visibile — solo `:l` lo faceva. Introdotta `editor_redraw(ctx, svc)` unica sorgente di verità della vista: fa `cls` + `ui_top_bar` + `ui_reserve_top_row(1)`, stampa status line `File: <nome>   Lines: <N>   [modified]|[clean]`, separatori visivi, buffer numerato a 3 colonne (`  1 | <testo>`), hint comandi e marker seriale nuovo `[edit] render lines=<N>`. `com_main` chiama `editor_redraw` subito dopo `load_file` success — così il contenuto è sempre visibile all'apertura. Aggiunti comandi: `:c` clear buffer, `:r` reload da disco, `:i N TEXT` inserisci prima della riga N, `:s N TEXT` sostituisci riga N. Tutti i mutatori marcano `g_dirty=1` e richiamano `editor_redraw`. `:l` e `:d N` ora auto-redisegnano. Top bar espansa per riflettere i nuovi comandi. Marker legacy (`[edit] open|save|quit …`) preservati verbatim; i testi "Opened … (N lines, M bytes)" e "New file: X" sono stati spostati dentro lo status line della redraw (altrimenti la `cls()` li avrebbe cancellati) — il marker seriale `[edit] open path=` resta intatto. Smoke esteso con due assert statiche nuove: presenza di `[edit] render lines=` e della chiamata `editor_redraw(ctx, svc);` nel sorgente. Validazione: `make all` PASS senza warning, `bash scripts/test_ciukedit_smoke.sh` PASS (static fallback), `CIUKIOS_SKIP_BUILD=1 TIMEOUT_SECONDS=60 make test-stage2` PASS (tutti i marker richiesti, nessun `#UD`/`[ panic ]`). Nessun bump di versione (`Alpha v0.8.7` invariata), nessun commit su `main`. Handoff completo in `docs/handoffs/2026-04-19-third-agent-ciukedit-completion-gui-003.md` (worktree-local, gitignored).

- Follow-up on same branch (stesso giorno, stesso task): aggiunti cursore + viewport + modalità scroll interattiva `:v` (arrow/PgUp/PgDn/Home/End/Esc/Enter) tramite `svc->int16` AH=00h; nuovi globali `g_cursor_line` / `g_viewport_top` con `clamp_viewport`, indicatore `>` nel gutter sulla riga corrente, status line estesa con `Cur: N`, comando `:g N` per salto rapido. Il plain-append sposta automaticamente il cursore sull'ultima riga (scroll naturale). Incluso file demo `assets/DANTE.TXT` (Inferno Canto I, una pagina, ASCII senza accenti) copiato da `run_ciukios.sh` come `::DANTE.TXT` nel root FAT — l'utente può lanciare `CIUKEDIT DANTE.TXT` e poi `:v` per scorrere con le frecce. Nuovi marker seriali `[edit] view enter` / `[edit] view exit`. Rivalidato: `make all` PASS, smoke PASS (static fallback), `test-stage2` PASS. surface.
---
## 2026-04-21 — Claude: VDI 0x6E semantic fix (vr_trnfm)

- Scoperta via sorgente OpenGEM (PPDV102.C + FUNCREF.DOC + ENTRY.A86): VDI opcode 110 (0x6E) e vr_trnfm (transform raster form), non vst_load_fonts (opcode 119).
- Commit f3a33d7 su wip/opengem-044b-real-v86-first-int corregge handler in stage2/src/v86_dispatch.c: n_intout=0, n_ptsout=0, no-op buffer transform.
- Effetto: GEM.EXE supera 172 chiamate VDI init, entra post-VDI phase (exec nested GEM.EXE, INT 21h 4E/3B/47/48/4A, set_vector INT EF).
- Next blocker: guest stalla in idle-loop post 36 INT 21h; QEMU consegna IRQ0 ma IVT vec 0x08 non e hookato. Prossimo step: installare IVT handler INT 08h (inc BDA 0040:006C + EOI 0x20->0x20).
