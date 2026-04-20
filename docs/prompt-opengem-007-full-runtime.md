# Prompt: OPENGEM-007 - Full Runtime Visual Launch

Sei un coding agent su CiukiOS. Esegui il task OPENGEM-007 per ottenere avvio completo e visualizzazione reale del desktop OpenGEM.

## Leggi prima
1. /Users/peronslayer/Downloads/CiukiOS/CLAUDE.md
2. /Users/peronslayer/Downloads/CiukiOS/docs/agent-directives.md
3. /Users/peronslayer/Downloads/CiukiOS/docs/task-opengem-007-full-runtime.md
4. /Users/peronslayer/Downloads/CiukiOS/docs/roadmap-opengem-ux.md
5. /Users/peronslayer/Downloads/CiukiOS/docs/opengem-runtime-structure.md

## Workflow obbligatorio
1. Lavora su branch dedicato, mai su main.
2. Nessun bump versione (resta Alpha v0.8.7).
3. Mantieni compatibilita dei marker OpenGEM esistenti.
4. Non rimuovere i test OpenGEM gia presenti.
5. Merge solo su esplicita richiesta utente: "fai il merge".

## Obiettivo tecnico
Chiudere il gap tra integrazione e runtime reale:
- OpenGEM deve risultare realmente avviato fino a desktop visualizzato.
- Runtime gate obbligatorio con log seriale e marker verificabili.
- Ritorno da OpenGEM verso shell/desktop senza corruzione stato.

## Implementazione richiesta
1. Aggiungi marker runtime granulari:
   - OpenGEM: runtime handoff begin
   - OpenGEM: desktop first frame presented
   - OpenGEM: interactive session active
   - OpenGEM: runtime session ended
2. Mantieni i marker storici gia usati dai gate attuali.
3. Crea script nuovo: scripts/test_opengem_full_runtime.sh
4. Integra target Makefile: test-opengem-full-runtime
5. Crea doc nuovo: docs/opengem-full-runtime-validation.md
6. Conferma restore stato e funzionamento exit chord ALT+G+Q

## Gate minimi da eseguire
1. make test-opengem-full-runtime
2. make test-opengem-smoke
3. make test-opengem-launch
4. make test-opengem-input
5. CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh

## Output finale richiesto
1. Elenco file modificati.
2. Evidenza marker runtime nel log seriale.
3. Risultati test (PASS/FAIL).
4. Rischi residui.
5. Prossimo step suggerito.

Importante: non fare merge automatico.
