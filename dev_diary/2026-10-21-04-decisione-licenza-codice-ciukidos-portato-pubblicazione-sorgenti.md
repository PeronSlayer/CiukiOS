# Decisione del proprietario: il codice CiukiDOS portato nel kernel DOS unificato è concesso in GPL-2.0-or-later; i sorgenti corrispondenti sono pubblicati su GitHub e in un archivio per release

- **Data:** 21 ottobre 2026
- **Tipo:** decisione (licenze, distribuzione)
- **Versione/commit:** 0.9.2 pre-alpha / main `2f62df67`
- **Stato:** in vigore; sblocca le direttive f3-56 e f3-58 del programma DOS unificato (voce 2026-10-21-03, contratto `docs/design/unified-dos-contract.md`)

## Decisioni

1. Il proprietario, titolare del codice di CiukiDOS (`src/boot/floppy_stage1.asm` e i moduli derivati), concede che le parti portate nel fork del kernel FreeDOS 1.3 (GPL-2.0-or-later) siano rilasciate con la stessa licenza GPL-2.0-or-later. La concessione è registrata in `docs/licenses/CiukiDOS-unified-kernel-GRANT.md`; il codice CiukiDOS che resta fuori dal fork non cambia licenza.
2. I sorgenti corrispondenti del kernel unificato (fork, patch, script di build, pin e avvisi) sono pubblicati nel repository GitHub di CiukiOS e, per ogni release che contiene il kernel, in un archivio sorgenti con hash SHA-256 registrato nel manifesto della release (f3-55 produce l'archivio; la URL immutabile è quella della release GitHub).

## Riferimenti

- Voci 2026-10-21-03 (unificazione), 2026-10-19-06; `docs/design/unified-dos-contract.md` (licenze), `docs/design/unified-dos-licenses.tsv`; direttive f3-55 (archivio sorgenti), f3-56 e f3-58 (porting).
