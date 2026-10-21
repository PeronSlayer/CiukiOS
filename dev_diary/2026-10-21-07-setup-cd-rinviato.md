# 2026-10-21-07 — Setup da CD rinviato a una release successiva

## Decisione

Il proprietario, alle 20:29 del 2026-10-21, ha rinviato il Setup da CD
(piano `docs/design/cd-setup-plan.md`, direttive f9-00…f9-06, approvato in
2026-10-21-06) a una release successiva alla 1.0 Alpha. La 1.0 Alpha viene
consegnata, come le immagini precedenti, come immagine disco
`build/hw/ciukios-hw.img` scritta sul disco di prova del T23; il supporto
ISO non è un prerequisito.

## Motivazione

Le direttive f9 (media CD, strumenti disco nativi, applicazione Setup)
assorbivano tre sessioni agente e la fusione di f9-01 richiedeva il
token `setup=1` nel loader e la risoluzione di nove punti aperti, senza
contribuire ai contenuti della 1.0 Alpha (DOS unificato, tier installer
Win32, HTTPS nel browser, budget dispositivi, transazioni FAT).

## Stato congelato

- `wt/f9-01` (`00075bea`): consegna f9-01 completa (build_cd.py, boot
  image El Torito, kernel `cdview`, suite f9-cd-smoke) più la fusione
  con `main` a `f162192b` e il token `setup=1` portato sulla tabella
  opzioni compatta di f0-54 (8 byte). Non fuso in `main`.
- `wt/f9-02` (`2a99edae`) e `wt/f9-03` (`aac5cb00`): lavoro parziale,
  agenti fermati; nulla fuso.
- Le direttive f9-00…f9-06 restano nell'indice con stato «rinviato».

La ripresa parte dalla fusione di `main` in `wt/f9-01` e dalla riapertura
delle nove decisioni elencate in `build/codex/f9-01-report.md`.
