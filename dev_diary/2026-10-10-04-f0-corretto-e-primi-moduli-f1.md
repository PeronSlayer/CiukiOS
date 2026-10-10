# Correzioni F0 integrate; primi moduli F1 su `main`

- **Data:** 2026-10-10
- **Tipo:** modifica
- **Versione/commit:** `da1bbe5` (f0-01), `e2f7023` (f1-01), `d580df5` (f1-05), `0865f20` (f1-04)
- **Stato:** completato su QEMU; integrazione dei moduli F1 nel runtime in corso

## Cosa è stato fatto

- **Correzioni f0-01** (Codex Sol): `core`/`all` si fermano alla prima sonda
  fallita e registrano le altre come `NOT_RUN`; cronologia dello schermo a
  1.536 righe; eco del selettore digitato a schermo; la modalità provvisoria
  ignora il modo video configurato; richieste del runner ricostruite in ordine
  canonico e `loader_options` rifiutate; predicati del timer legati alla
  formula del kernel; parser del kernel che rifiuta byte non stampabili, nomi
  sconosciuti e suffissi senza i flag di avvio.
- **Evidenza QEMU** (profili icount T23, E500, 128 MiB): sull'immagine del
  worktree `ba8ea695…` e sull'immagine fusa `6444ad6b…` (commit `e2f7023`)
  `f0-smoke` 2/2, `f0-core` 56/56 (il caso safe-mode è ora automatico),
  `f0-panic` con il caso UART-assente che per contratto richiede conferma
  visiva. Record: `docs/validation/2026-10-10-f0-review-fixes.md`.
- **Moduli F1 integrati su `main`**, non ancora collegati al runtime:
  - f1-01 (Codex Astra): FAT12/16/32 con nomi lunghi, cache write-back con
    barriere, partizioni MBR/EBR, VFS con handle e share mode; interfaccia
    `blkdev` congelata per il driver ATA. Suite host: 14 gruppi, 86.673
    controlli, 2.106 tagli di crash senza cross-link, `fsck.fat` e `mtools`
    concordi su FAT12/16/32. Limiti registrati in `tests/host/fs/REPORT.md`.
  - f1-05 (Codex Sol): dispositivo framebuffer e presenter intero a 24/32 bpp
    con clipping, verificato contro un oracolo (15.264 confronti).
  - f1-04 (Codex Sol): driver i8042 nativo con coda eventi, decodifica set 2
    e mouse, quarantena, rifiuto in modalità firmware-first; 76.292 controlli
    host; lo stimolo del contratto produce 100 caratteri, 200 transizioni di
    tasto, 20 di pulsante, x=200, y=-100.
- **Direttive emesse**: f1-03 (fusione di f1-02 con `main`, dispatch F1 e
  registrazione delle sonde via sezione del linker), f1-06 (ATA PIO, viste
  di partizione, SHA-256, sonde `ata`/`ata-fault`/`partition`), f1-07 (BIOS
  VM V86 serializzata e backend input firmware-first per l'E500), f2-00
  (contratto F2: sottoinsieme POSIX, processi, SDK, accettazione).

## Decisioni

- Scadenza delle risposte PS/2 native: vale il contratto (200 ms), non i
  100 ms della direttiva; da correggere all'integrazione.
- Nessuna variante di build per forzare una sonda fallita: il percorso
  `NOT_RUN` resta coperto dai test host finché non si presenta sull'hardware.
- I moduli F1 vengono collegati al runtime da una direttiva d'integrazione
  dopo f1-03, f1-06 e f1-07 (avvio dei driver, registrazione delle sonde,
  `boot-framebuffer` come riserva firmware nel registro, stack protector).
