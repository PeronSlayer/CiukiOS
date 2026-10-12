# Fusione di wt/integ2 in main: pre-alpha 0.9.0 integrata

- **Data:** 2026-10-12
- **Tipo:** fusione; decisione del proprietario
- **Versione/commit:** fusione di `wt/integ2` (`4d4a39bf`) in `main`
- **Stato:** su `main` e pubblicato su `origin`

## Decisione

Il proprietario ha interrotto la campagna QEMU residua (desktop/app,
processi, DPMI/video/finestra DOS, DOS/4GW/DOOM/Wolf3D, audio sull'immagine
`58c835bd…`) e ha chiesto di completare la fusione in `main` con le prove
disponibili, provando l'immagine direttamente sul T23. La pubblicazione
delle prerelease resta sospesa (`config/release-policy.json`).

## Prove disponibili per l'immagine `58c835bd…` (albero `91ef115f`)

`f0-smoke` 4 PASS; `f2-desktop` 186 PASS (prefisso F0/F1 su cinque profili
e isolamento dei crash del desktop); `uart-absent-*` casi da operatore;
`desktop-shell-qemu-t23` fallito per il controllo del ritratto del runner,
corretto in `4d4a39bf` (il controllo segue il ritratto nel pulsante Ciuki).
Le prove dei giochi, DPMI, processi e audio valgono per le immagini
precedenti della giornata (voci 2026-10-12-01…04) e vanno ripetute su
un'immagine di `main` alla prossima campagna.

Immagine hardware consegnata: `build/hw/ciukios-hw.img` SHA-256
`569b3fca1c503a0a71c45275051b9d433dc1aad0ea069cf3ca74cf87aadca444`.

## Aperti

Campagna QEMU completa su un'immagine di `main`; direttiva `runner-01`
(slot paralleli); decisione sul pacchetto Wolfenstein 3D shareware.
