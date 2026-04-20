# Task: OPENGEM-007 - Visualizzazione e Avvio Completo Runtime

## Obiettivo
Portare OpenGEM da integrazione "launch path + marker" a esecuzione completa verificata:
- desktop OpenGEM effettivamente visualizzato
- sessione interattiva stabile (input base funzionante)
- uscita controllata e ritorno coerente a shell/desktop CiukiOS

Questo task e una estensione post OPENGEM-001..006 (gia chiusi) e copre il gap residuo tra test statici e validazione runtime reale.

## Contesto Verificato (stato attuale)
OpenGEM risulta gia integrato in main con:
- launcher shell/desktop/ALT+O
- compatibilita BAT rinforzata
- catalog e bridge input/mouse
- test scripts dedicati

Tuttavia i gate principali OpenGEM sono prevalentemente statici o fixture-gated. Serve una prova runtime deterministica della visualizzazione completa del desktop OpenGEM.

## Scope

### In scope
1. Runtime boot path OpenGEM fino a desktop visualizzato.
2. Marker seriali runtime aggiuntivi per distinguere:
   - invocazione launcher
   - handoff a GEM
   - primo frame desktop GEM presentato
   - sessione interattiva attiva
   - uscita e ritorno a CiukiOS
3. Test end-to-end non solo statico:
   - avvio controllato in QEMU
   - cattura log seriale
   - verifica marker runtime obbligatori
4. Verifica input minima durante sessione GEM:
   - tastiera (almeno chord di uscita)
   - mouse (se disponibile) senza corruzione stato al ritorno
5. Fallback chiaro quando payload OpenGEM non e disponibile.

### Out of scope
1. Audio completo per OpenGEM/DOOM.
2. Ottimizzazioni avanzate VDI non necessarie al primo desktop stabile.
3. Nuove feature DOOM path oltre ai marker gia presenti.

## Deliverable richiesti
1. Aggiornamento launcher/runtime code in stage2 per marker runtime granulari.
2. Nuovo test gate runtime obbligatorio:
   - `scripts/test_opengem_full_runtime.sh`
   - target Makefile: `test-opengem-full-runtime`
3. Documento operativo:
   - `docs/opengem-full-runtime-validation.md`
4. Handoff locale in `docs/handoffs/` (seguendo policy repo locale).

## Piano implementativo

### Step 1 - Baseline runtime
- Mappare il punto esatto in cui la sessione OpenGEM passa da preflight a esecuzione reale.
- Individuare punto robusto per marker "desktop pronto" (non solo launch requested).

### Step 2 - Marker runtime nuovi (frozen)
Aggiungere marker seriali minimi:
1. `OpenGEM: runtime handoff begin`
2. `OpenGEM: desktop first frame presented`
3. `OpenGEM: interactive session active`
4. `OpenGEM: runtime session ended`

Nota: mantenere anche i marker storici gia usati dai test esistenti.

### Step 3 - Gate runtime E2E
Implementare `scripts/test_opengem_full_runtime.sh` con due livelli:
1. static sanity (veloce)
2. runtime mandatory:
   - avvio QEMU con timeout
   - log seriale obbligatorio
   - assert dei marker nuovi + marker storici critici

Il gate deve fallire se manca la prova runtime (niente SKIP silenzioso su path principale del task).

### Step 4 - Stabilita ritorno sessione
- Verificare restore stato desktop/shell dopo uscita da OpenGEM.
- Verificare che il fallback cursor/mouse non resti in stato incoerente.
- Verificare che ALT+G+Q continui a funzionare come uscita di sicurezza.

### Step 5 - Documentazione
Creare `docs/opengem-full-runtime-validation.md` con:
- prerequisiti
- comando di test
- marker attesi
- esempi di failure comuni e fix rapidi

## Definition of Done
Tutte le voci devono risultare PASS:

1. OpenGEM si avvia fino a desktop visibile (evidenza log runtime).
2. Marker runtime nuovi presenti e ordinati correttamente nel log.
3. `bash scripts/test_opengem_full_runtime.sh` ritorna 0.
4. `make test-opengem-full-runtime` ritorna PASS.
5. Regressioni assenti su gate correnti:
   - `make test-opengem-smoke`
   - `make test-opengem-launch`
   - `make test-opengem-input`
6. Build macOS con skip-run resta verde:
   - `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh`
7. Nessun bump versione (baseline invariata).

## File candidati da toccare
- `stage2/src/shell.c`
- `stage2/src/stage2.c` (solo se necessario per telemetria/hook)
- `scripts/test_opengem_full_runtime.sh` (nuovo)
- `Makefile`
- `docs/opengem-full-runtime-validation.md` (nuovo)
- `docs/handoffs/YYYY-MM-DD-opengem-007-full-runtime.md` (locale)

## Rischi noti
1. Ambiente macOS con QEMU puo introdurre flakiness temporale.
2. Se la telemetria non distingue bene launch vs frame present, il gate puo dare falsi positivi.
3. Eventuali limiti DOS extender/VDI possono emergere solo a runtime reale.

## Criterio di priorita
Questo task e prioritario prima di ulteriori evoluzioni UX, perche chiude il requisito utente fondamentale: vedere OpenGEM partire davvero in modo completo e verificabile.
