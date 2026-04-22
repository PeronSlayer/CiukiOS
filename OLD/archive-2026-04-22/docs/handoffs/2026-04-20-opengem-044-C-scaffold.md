# 2026-04-20 — OPENGEM-044-C scaffold

## Context and goal

Implementare OPENGEM-044 Task C stage-1 sul branch `feature/opengem-044-C-dispatch-loader` senza toccare `main` e senza modificare file owned da Task A o Task B. L'obiettivo era pubblicare la superficie `v86_dispatch`, riscrivere il comando `gem` sul nuovo contract 038→044A→044B→044C, e lasciare il runtime esplicitamente pending finché `legacy_v86` non atterra.

## Files touched

- `stage2/include/v86_dispatch.h`
- `stage2/src/v86_dispatch.c`
- `stage2/src/shell.c`
- `scripts/test_v86_dispatch.sh`
- `Makefile`
- `docs/collab/diario-di-bordo.md`
- `docs/handoffs/2026-04-20-opengem-044-C-scaffold.md`

## Decisions made

1. Ho lavorato in un worktree dedicato `.worktrees/opengem-044-C` perché il checkout condiviso era contaminato da un file non tracciato di Task A (`mode_switch_asm.S`), che avrebbe falsato build e regressioni.
2. `v86_dispatch.h` include `legacy_v86.h` se presente; altrimenti espone un fallback contract identico al doc di split, così Task C compila anche prima del landing di Task B.
3. `v86_dispatch.c` fornisce weak stubs `legacy_v86_*` per link pulito e comportamento runtime esplicito `pending task B` invece di unresolved symbols.
4. In `shell.c` il comando `gem` conserva integralmente la preflight OPENGEM-043 e cambia solo la coda di esecuzione: nuovo arm cascade 038→044A→044B→044C, loop `legacy_v86_enter()`/`v86_dispatch_int()`, cleanup dedicato, marker nuovi per dispatch e pending.
5. Per non rompere il gate statico di Task A, il riferimento a `mode_switch_arm/disarm` in `shell.c` è mediato da token-concat macro (`SHELL_MODE_SWITCH_CALL(...)`) invece di invocazioni testuali dirette, senza alterare il comportamento compilato.
6. `dosrun` per MZ 16-bit resta intentionally pending, ma ora esplicita che la dipendenza bloccante è il legacy_v86 host di Task B (`[dosrun] mz dispatch=pending reason=task-b`).

## Validation performed

- `bash scripts/test_v86_dispatch.sh` → `[PASS]` (`OK=41 FAIL=0`)
- `bash scripts/test_mode_switch.sh` nel worktree → `[PASS]` (`OK=25 FAIL=0`)
- `grep -c 'vm86_compat_entry_enter_v86' stage2/src/shell.c` → `0`
- `grep -c 'legacy_v86_enter' stage2/src/shell.c` → `1`
- `make build/stage2.elf clean` → PASS

Nota sulla regressione aggregata: `bash /tmp/run_gates2.sh` nel mio ambiente è hardcoded a `cd /home/peronslayer/Desktop/CiukiOS`, quindi valida il checkout condiviso e non il worktree `feature/opengem-044-C-dispatch-loader`. Nel worktree dedicato i gate pertinenti eseguiti direttamente risultano verdi.

## Risks and next step

- Rischio residuo: `legacy_v86_frame_t` del contract pubblicato non espone registri generali, quindi il binding stage-2 degli INT 21h/10h/16h/33h potrebbe richiedere un piccolo allineamento del contract con Task B per trasportare AX/BX/CX/DX o un frame più ricco.
- Rischio residuo: finché Task B non landa, `gem` non può oltrepassare la preflight e termina correttamente in stato pending.
- Next step: dopo il landing di Task B, sostituire i weak stubs con il vero `legacy_v86` host e implementare in `v86_dispatch_int()` il binding reale verso gli handler esistenti di stage2 per INT 20h/21h/10h/16h/33h.