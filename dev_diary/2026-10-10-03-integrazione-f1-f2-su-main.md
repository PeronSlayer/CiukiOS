# F1 e F2 si integrano su `main`; prova hardware unica da F0 a F2

- **Data:** 2026-10-10
- **Tipo:** decisione (processo)
- **Versione/commit:** commit di questa voce
- **Stato:** decisa dal proprietario

## Contesto

La regola di transizione (`docs/design/foundations-transition.md`) vietava di
integrare su `main` il codice di un passo finché il gate del passo
precedente era aperto; F0 ha ancora aperta la qualificazione sull'hardware
(T4). Il primo risultato F1 (servizi del kernel, direttiva f1-00) era
pronto e rivisto.

## Decisione

1. F1 e F2 vengono integrati direttamente su `main` appena le loro evidenze
   QEMU passano; nessun ramo d'integrazione separato.
2. La qualificazione fisica di F0, F1 e F2 avviene **una sola volta**, sulla
   stessa immagine, quando F2 è chiuso su QEMU (T23 ed E500).
3. Nessun passo viene dichiarato completo prima di quella prova; i gate QEMU
   dei passi precedenti devono continuare a passare su ogni immagine
   integrata.
4. Ruoli confermati: Codex scrive tutto il codice (anche il collante);
   Claude dirige, rivede, integra ed esegue le prove QEMU.

## Cosa è stato fatto

- `main` avanzato con la direttiva f1-00 (servizi del kernel: mutex
  dormiente, code di attesa, lavoro differito, handle con generazione, ciclo
  di vita del registro con quarantena, catene IRQ PCI condivise) e con il
  collante in `task.c`/`probes.c` (uscita dei task, avvio del worker).
  Test host 6/6 e build del kernel con audit FPU superati.
- Regola aggiornata in `foundations-transition.md` e `AGENTS.md`.
