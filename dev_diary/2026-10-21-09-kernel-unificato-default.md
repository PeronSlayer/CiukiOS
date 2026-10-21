# 2026-10-21-09 — Kernel DOS unificato come predefinito

## Decisione

Il proprietario, alle 23:20 del 2026-10-21, ha deciso che il kernel DOS
unificato (fork CiukiDOS di FreeDOS 2043 con binding DFV2 e IFS nativo,
f3-56/f3-57) è il kernel predefinito dei giochi DOS nella 1.0.0 alpha.

## Attuazione

- `apps/dosbox/settings.h`: un gioco senza impostazione salvata, o con un
  valore sconosciuto, usa `CIUKI_DOSVM_UNIFIED`; `DOS kernel=ciukidos`
  seleziona esplicitamente il kernel classico; etichette
  «CiukiDOS unified (default)» e «CiukiDOS classic».
- La shell del DOS Box (nessun gioco esplicito) resta sul kernel classico:
  l'ABI (`abi.h`, CREATE) consente l'override solo per un gioco; il passaggio
  della shell al kernel unificato è l'oggetto di f3-58.
- L'immagine installa il fork solo con `build/unified-dos/fork-qualified.json`
  prodotto da `tests/host/dos/run_unified_file.sh`; la catena finale si
  interrompe se il record manca.
