# Reset architetturale verso BIOS legacy x86

- **Data:** 2026-04-22 – 2026-05-03
- **Tipo:** retrospettiva
- **Versione/commit:** pre-Alpha 0.5.0 → 0.5.9-final, 0.5.2 – 0.5.4
- **Stato:** completato

## Contesto

Decisione: abbandonare UEFI/x86_64 e ripartire da un'architettura BIOS legacy
a 16 bit, eseguendo i programmi DOS sulla CPU reale.

## Cosa è stato fatto

- Boot sector, catena stage0 → stage1, profili `floppy` (FAT12) e `full` (FAT16).
- DOS in modalità reale nello Stage1: INT 21h di base, PSP, loader COM e MZ,
  I/O su handle, MCB, DTA e ricerca file, unità e directory corrente.
- Primo strato grafico VGA 13h e un livello simile a VDI verso il desktop.
- Desktop runtime (OpenGEM) avviato e tornato alla shell: 20 sessioni QEMU,
  soak di 100 × 20 minuti e una prova su PC fisico (24 aprile).
- Scheletro dell'installer e del profilo CD.
- Fase 3 chiusa il 30 aprile.

## Verifica

Gate QEMU deterministici con marcatori seriali; una prova hardware su PC
fisico per il desktop runtime.

## Riferimenti

- `docs/diario-bordo-v2.md`, voci 1–89
- `docs/architecture-legacy-x86-v1.md`
- [`CHANGELOG.md`](../CHANGELOG.md), sezioni 0.5.x
