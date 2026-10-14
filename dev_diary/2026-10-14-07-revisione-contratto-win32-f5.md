# Revisione incrociata del contratto Win32 F5

- **Data:** 2026-10-14
- **Tipo:** analisi
- **Versione/commit:** `4bc615b50bbc6b9163087607513abc9f55b57da3`
- **Stato:** revisione completata; disposizioni del responsabile da registrare

## Contesto

Il contratto F5 definisce l'installazione e l'esecuzione dei giochi Windows
del periodo 1996–2004 mediante una personalità Win32 sul kernel CiukiOS.
La revisione confronta la bozza con fonti primarie e con gli attuali contratti
di esecuzione, POSIX, desktop, audio, registro e font.

## Cosa cambia

Una nuova sezione finale del contratto conserva la revisione completa senza
modificare il testo precedente. Il rapporto identifica 27 contraddizioni o
requisiti da precisare, otto deroghe o affermazioni da qualificare e 16 gruppi
di funzionalità o evidenze mancanti. Propone dipendenze senza cicli per
f5-02–f5-18, includendo la f5-19 già richiesta per gli installer Win16, e
strategie di verifica host su Linux.

La raccomandazione mantiene come proposta verificabile l'opzione ibrida già
approvata: audit delle dipendenze Wine prima del congelamento delle interfacce
e verifica integrata di un flusso installer. Non cambia le decisioni del
proprietario né dichiara approvato un nuovo ABI.

## Verifica

Lettura dei documenti, ricerca sul web e confronto con intervalli mirati del
codice esistente. Il testo originario del contratto è preservato byte per byte.
Nessun codice, build, test runtime, QEMU, esperimento Windows o prova hardware.
Il rapporto distingue le fonti accessibili dalle parti degli archivi SDK/DDK
non recuperate e le ipotesi sui giochi dalle API effettivamente documentate.

## Riferimenti

- [Revisione nel contratto Win32](../docs/design/win32-personality-contract.md#codex-cross-review-of-f5-00-2026-10-14).
- [Rapporto richiesto](../build/codex/f5-00-review-report.md), artefatto locale.
- Fonti primarie, limiti dell'evidenza e contratti confrontati sono citati
  nel rapporto e nella nuova sezione del contratto.
