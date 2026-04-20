# Prompt: OPENGEM-001 - OpenGEM Launcher Integration

Sei un coding agent su CiukiOS. Esegui il task OPENGEM-001 (Phase 1 del roadmap OpenGEM UX).

## Prerequisiti

Leggi in ordine PRIMA di qualsiasi implementazione:
1. `/Users/peronslayer/Downloads/CiukiOS/CLAUDE.md`
2. `/Users/peronslayer/Downloads/CiukiOS/docs/agent-directives.md`
3. `/Users/peronslayer/Downloads/CiukiOS/docs/collab/diario-di-bordo.md` (file locale, controlla overlap)
4. `/Users/peronslayer/Downloads/CiukiOS/docs/task-opengem-001-launcher.md` (task completo)
5. `/Users/peronslayer/Downloads/CiukiOS/docs/roadmap-opengem-ux.md` (contesto strategico)

## Workflow Obbligatorio

1. **Branch:** Crea e usa branch dedicato `feature/opengem-001-launcher`. NON LAVORARE SU MAIN.
2. **Non fare bump versione:** Baseline rimane CiukiOS Alpha v0.8.7.
3. **No breaking changes:** ABI stage2/loader/shell rimane stable.
4. **Testing gates:** Mantieni PASS:
   - `make test-stage2`
   - `make test-gui-desktop`
   - Build macOS pipeline completa (skip-run ok)
5. **Documentazione:** Aggiorna `documentation.md` se stato architetturale cambia. Crea handoff per major change.
6. **Diario locale:** Aggiorna `docs/collab/diario-di-bordo.md` a task completato (file non tracciato).
7. **Merge:** Solo su esplicita richiesta utente "fai il merge".

## Obiettivo Tecnico

Implementare Phase 1 del roadmap OpenGEM UX:
- Validare runtime OpenGEM è funzionante
- Creare smoke test
- Aggiungere launcher button nel desktop scene
- Wiring ALT+O per launch
- Gestire transizioni desktop → OpenGEM → shell
- Documentare e testare

## Scope Esatto

### In Scope
1. Analizzare `third_party/freedos/runtime/OPENGEM/` e documentare structure
2. Creare `scripts/test_opengem_smoke.sh` che valida runtime e boot
3. Desktop scene: aggiungere launcher button/icon per OpenGEM
4. Wiring: ALT+O → launch_opengem()
5. State machine: desktop ↔ OpenGEM ↔ shell transitions con save/restore
6. Serial debug markers per boot/launch/exit flow
7. Handoff completo con implementation details

### Out of Scope
1. App discovery / file browser (Phase 3)
2. Advanced mouse/keyboard testing (Phase 4)
3. DOOM binary integration (Phase 5)
4. OpenGEM customization o configurazione avanzata

## Implementazione Step-by-Step

### 1. Analisi Runtime (1-2 ore)
- Ispeziona `third_party/freedos/runtime/OPENGEM/`
- Identifica GEM.BAT, DESKTOP.APP, config files
- Documenta dipendenze e prerequisiti
- Crea file: `docs/opengem-runtime-structure.md`

**Deliverable:** Documento chiaro su layout e entry points.

### 2. Smoke Test Script (1-2 ore)
- Crea `scripts/test_opengem_smoke.sh`
- Valida runtime files esistono
- Boot OpenGEM con timeout 30-60s
- Cerca marker seriale "OpenGEM: launcher window initialized"
- Report PASS/FAIL
- Integra in Makefile: `make test-opengem-smoke`

**Deliverable:** Script testabile e gate automatico.

### 3. Serial Debug Markers (30 min)
- Aggiungi log in stage2.c: "OpenGEM: boot sequence starting"
- Marca launcher init: "OpenGEM: launcher window initialized"
- Marca exit: "OpenGEM: exit detected, returning to shell"
- Verifica in serial console

**Deliverable:** Marker visibili per troubleshooting.

### 4. Desktop UI Integration (2 ore)
- Localizza desktop scene code (`stage2/src/ui.c` o `stage2/src/desktop.c`)
- Aggiungi launcher button/icon visibile (dock area o bottom bar)
- Button click → launch_opengem()
- Wiring ALT+O keyboard shortcut alla stessa funzione
- Save desktop state prima launch, restore dopo return

**Deliverable:** Button visibile e funzionante nel desktop.

### 5. Launch Function (1-2 ore)
- Funzione: `shell_run_opengem_interactive(boot_info, handoff)`
- Localizza entry point OpenGEM in FAT (GEM.BAT o DESKTOP.APP)
- Save shell state (cwd, environment variables)
- Call shell_run() con OpenGEM entry
- Capture exit reason e status
- Restore shell state, ritorna al desktop

**Deliverable:** Launch/return flow robusto.

### 6. Testing e Validation (1 ora)
- Esegui `make test-opengem-smoke` → PASS
- Esegui `make test-stage2` → PASS (no regressions)
- Esegui `make test-gui-desktop` → PASS (no regressions)
- Build macOS completa: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → PASS
- Desktop launcher button still responds to ESC / ALT+G+Q

**Deliverable:** Tutti i gate verdi, no regressions.

### 7. Documentation & Handoff (1 ora)
- Aggiorna `documentation.md` con sezione OpenGEM
- Crea handoff: `docs/handoffs/2026-04-19-opengem-001-launcher.md`
- Documento contenuti:
  - Context + goal
  - Files touched (elenco)
  - Decisions fatte (design choices)
  - Validation performed (test results)
  - Risks residui + next steps
- Aggiorna `docs/collab/diario-di-bordo.md` con note locali

**Deliverable:** Documentazione e handoff completi.

## Definition of Done

Checklist finale (TUTTE le voci PASS):

- ✅ OpenGEM runtime structure documented in `docs/opengem-runtime-structure.md`
- ✅ `bash scripts/test_opengem_smoke.sh` esecuzione PASS
- ✅ Desktop scene includes launcher button per OpenGEM (visibile e clickable)
- ✅ ALT+O (o shortcut assegnato) launches OpenGEM reliably
- ✅ State save/restore funzionante: desktop ↔ OpenGEM ↔ shell transitions smooth
- ✅ Serial debug markers present: boot/launch/exit markers in console
- ✅ `make test-stage2` PASS (no regressions)
- ✅ `make test-gui-desktop` PASS (no regressions)
- ✅ Build macOS pipeline: `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` PASS
- ✅ Handoff file created: `docs/handoffs/2026-04-19-opengem-001-launcher.md`
- ✅ `documentation.md` updated con OpenGEM section
- ✅ `docs/collab/diario-di-bordo.md` updated (local, untracked)
- ✅ Branch `feature/opengem-001-launcher` all changes staged, ready for review
- ✅ No uncommitted changes
- ✅ No version bump (CiukiOS Alpha v0.8.7 baseline preserved)

## Fallback Scenarios

Se OpenGEM missing/unavailable:
- Graceful fallback: desktop button disabled or shows "OpenGEM not available"
- Log seriale: "OpenGEM: runtime not found in FAT, fallback to shell"
- Test smoke: check exit code or fallback marker instead di launcher window

Se desktop state corruption:
- Explicit save/restore con checksum validation
- Panic marker seriale se state restore fails
- Revert to clean desktop initialization se recovery fails

## Validation Checklist

Prima di concludere:

1. **Runtime validation:** `third_party/freedos/runtime/OPENGEM/` exists e contains required files
2. **Test execution:** `bash scripts/test_opengem_smoke.sh` returns 0 (PASS)
3. **Visual check:** Desktop launcher button visible in scene at boot
4. **Functional test:** ALT+O (or configured shortcut) launches OpenGEM window
5. **State test:** Desktop state preserved after return from OpenGEM
6. **Regression test:** `make test-stage2` and `make test-gui-desktop` both PASS
7. **Build test:** Full macOS pipeline completes successfully
8. **Documentation:** All three docs (runtime-structure, documentation.md, handoff) complete
9. **Git state:** Branch clean, all changes staged, ready for user merge approval

## Output Summary

Upon completion, provide:

1. **Code Changes:** List files modified + brief justification
2. **Test Results:** Command output from each test gate (copy-paste relevant parts)
3. **Serial Markers:** Example serial console output showing boot/launch/exit markers
4. **Screenshots/Observations:** If visual changes, describe desktop button appearance
5. **Risks Identified:** Any edge cases or fallback scenarios discovered
6. **Next Steps:** What Phase 2 will need from Phase 1 artifacts

**IMPORTANTE:** Non fare merge automatico. Attendi richiesta esplicita utente "fai il merge".
