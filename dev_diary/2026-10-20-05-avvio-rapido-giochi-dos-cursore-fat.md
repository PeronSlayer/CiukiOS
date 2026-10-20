# Avvio rapido dei giochi DOS (f3-51c) e cursore del file di scambio (f0-49b)

- **Data:** 2026-10-20
- **Tipo:** modifiche (storage, VM DOS, memoria)
- **Versione/commit:** 0.9.2 pre-alpha / main (fusioni `abd1c7c1` f3-51c, f0-49b); immagine QEMU `1fba362e582cd41a338fc77027552ab850e654d7448378fc0b40c0e9f2b6fd09` per la verifica di f3-51c
- **Stato:** integrato con suite host verdi; verificato su QEMU (f3-51c)

## Fatti

1. **f3-51c** (agente Claude Opus): cursore di cluster per file aperto nel percorso FAT, validato da un timbro della catena rinnovato a ogni modifica (estensione, troncamento, rilocazione, cancellazione, rename sopra un file esistente); generazioni di scrittura nel VFS e `vfs_handle_version`; cache LRU di 16 impronte SHA-256 delle basi dei giochi, indicizzata dalla versione della base. Lettura sequenziale di un file da 27.336.704 byte su FAT32: da 2.801.342 a 13.295 consultazioni della FAT (lineare); riavvio con base invariata: nessuna lettura. Modello: il cluster corrente per file aperto del codice sorgente di MS-DOS 4.0.
2. **Verifica su QEMU** (TCG pentium3, 512 MiB): DOOM da Programmi > Giochi mostra la schermata di avvio entro 15 s dal clic (prima 60–90 s) e la demo a 30 s; la finestra si chiude correttamente; il secondo avvio è alla schermata di avvio entro 10 s.
3. **f0-49b** (agente Claude Opus): il cursore di cluster del file di scambio era validato solo da primo cluster e indice e non veniva azzerato da un taglio in coda; dopo una crescita interrotta, taglio e nuova crescita potevano mappare un cluster ormai di un altro file. Ora usa il timbro di f3-51c ed è svuotato a ogni errore; test `trim_regrow_reuse` con controllo negativo (senza la correzione la crescita mappa tutti i 4096 slot sul file `REUSED.BIN`).
4. Suite fs, kernel (con `pager_test` a rilevamento dei leak attivo), DOS (49 test), `make CAP= apps` verdi.

## Decisioni

1. Accettate le estensioni di f3-51c oltre l'elenco della direttiva (cursori anche per i descrittori di file nativi e Win32 e per il caricatore ELF; 5.648 byte in più per istanza del VFS, una pagina in più per il runtime DOS di ogni VM).

## Riferimenti

- `docs/directives/f3-51c-*.md`, `f0-49-*.md` (seguito f0-49b), `f3-51-*.md` (emendamento 2); `build/codex/f3-51c-report.md`, `f0-49b-report.md`; voce 2026-10-20-04.
