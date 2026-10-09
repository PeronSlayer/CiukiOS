# Report dei log fisici, diskseq 39

- **Data:** 2026-10-09
- **Tipo:** analisi
- **Versione/commit:** immagine R17D
- **Stato:** completato (nessun fix avviato)

## Contesto

Raccolta in sola lettura dei log scritti dal sistema sul disco
TS64GMSA230S dopo le prove sul T23 e sull'E500. Il kernel e i componenti
VM coincidono byte per byte con l'immagine R17D.

## Esito

**Confermato**
- E500: input funzionante, desktop VBE 1024×768 a 24 bit, ATI Mach64 rilevata
  senza accelerazione, audio ESS non supportato.
- T23: la qualificazione S3 supera il riempimento ma fallisce la copia 2D
  (stage 10), prima del test 3D.
- Prima sessione DOOM chiusa con codice VM 0; al secondo tentativo il
  puntatore DA (IVT 0368h) è `0000:0000`.
- Audio dei giochi: 101 attese oltre limite, servizio fino a 12,224 ms,
  nessun arresto DMA o errore FIFO.

**Non confermato**
Causa della copia S3, causa dell'azzeramento di DA, origine della distorsione
audio, prova ATI sull'E500, fluidità misurata e tempo di caricamento dello
sfondo.

## Riferimenti

- Report completo: `docs/validation/2026-10-09-diskseq39-log-report.txt`
- Log grezzi: conservati solo localmente, non pubblicati
- Analisi architetturale che ne deriva: [revisione architettura](2026-10-09-02-revisione-architettura.md)
