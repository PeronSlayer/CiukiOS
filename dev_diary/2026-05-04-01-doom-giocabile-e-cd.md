# DOOM giocabile, installer e live/install CD

- **Data:** 2026-05-04 – 2026-05-17
- **Tipo:** retrospettiva
- **Versione/commit:** pre-Alpha 0.6.1, 0.6.3, 0.6.5, 0.6.6
- **Stato:** completato nel perimetro registrato

## Cosa è stato fatto

- **DOOM giocabile** (Fase 4, 4 maggio): DOS/4GW, WAD, rendering e gameplay
  sul profilo FAT16; confermato manualmente dal proprietario.
  Correzioni a loader MZ, read/seek FAT16, PSP/MCB, strategia di memoria e XMS.
- Installer chiuso nel suo perimetro (sezioni A–F) con matrice di scenari.
- Profilo **full-CD** promosso a supporto live/install: avvio El Torito,
  SETUP grafico, FORMAT.COM, installazione distruttiva su HDD.
- Prime correzioni su **ThinkPad T23 reale** durante l'installazione:
  solo EDD invece di CHS, batch di 8 settori, reset periodico INT 13h e infine
  scrittura su disco con I/O ATA diretto, aggirando il BIOS.
- Avvio della separazione Stage1/runtime (`\SYSTEM\RUNTIME.BIN`).
- Indagini sull'audio SB16 di DOOM.

## Lezione

Già qui il BIOS reale del T23 si comportava diversamente da SeaBIOS:
la soluzione stabile è stata un driver ATA diretto invece della dipendenza
dal BIOS a runtime.

## Riferimenti

- `docs/phase4-doom-gameplay-playable-2026-05-04.md`
- [`CHANGELOG.md`](../CHANGELOG.md), sezioni 0.6.x
