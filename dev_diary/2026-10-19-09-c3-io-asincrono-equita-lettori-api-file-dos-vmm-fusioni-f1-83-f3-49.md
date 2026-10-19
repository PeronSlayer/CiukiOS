# C3 del contratto Windows 98/ME (I/O asincrono ed equità dei lettori) e API file DOS servita dal VMM: fusioni f1-83 e f3-49 con riconciliazione del VFS

- **Data:** 2026-10-19
- **Tipo:** integrazione (storage, VFS, VM DOS)
- **Versione/commit:** 0.9.0 pre-alpha / main `9d85d7eb` (fusioni `c6a7801a` f3-49, `9d85d7eb` f1-83)
- **Stato:** integrato su main; qualificazione hardware rinviata (mandato 2026-10-19-07)

## Fatti

1. **f1-83 (C3, richieste asincrone).** Pool residente di 32 richieste (420 byte ciascuna, 16 KiB contabilizzati nel ledger come `MM_IO_REQUEST`), code per controller (canali ATA, dispositivi USB mass storage) con servizio per priorità/scadenza/FIFO/età, fusione di richieste adiacenti e suddivisione adattiva, dipendenze finite per flush e sovrapposizioni, recinzione per generazione delle partizioni, DMA a vettore di pagine con ripiego su bounce, facciate sincrone del VFS invariate; nel VFS separazione lettori/mutazioni con riacquisizione ordinata delle prenotazioni. Replay C3 (copia da 10 MiB a 1 MiB/s con lettore da 16 KiB ogni 20 ms, cache 8 MiB): latenza massima del lettore da 114–352 ms a ≤ 10,4 ms, zero scadenze mancate nelle sette varianti (cold-hot, frammentata, cache tutta sporca, sorgente USB, fsync concorrente, errore di lettura, cancellazione), throughput entro ±10 %, 14 verifiche fsck/mtools. Suite fs, kernel, USB, storage verdi; VMM.ELF 6.441.300 byte.
2. **f3-49 (API file DOS nel VMM, consegna parziale accettata).** Selettori INT 21h serviti dal VMM: 0D 0E 19 1A 1B 1C 2F 36 39–47 (incl. 43/00-01, 44/00,06–0B,0E) 4E 4F 56 57/00-01 59 5A 5B 5C 60 67 68 6C e INT 2Fh 1000h; SFT con regole JFT/PSP (20/232), nodi comuni FAT/VFS con matrice dei lock 15×15 (DOS 2–6.22, tabella 01403), INT 24h con Retry/Fail/Abort, findfirst/next con DTA da 43 byte protetta, disposizioni 6C, catena dei driver di dispositivo e sottoinsieme IOCTL, tracciamento. Harness H49 con sanitizer: 5.570.788 asserzioni, 100 cicli di vita, 17 punti di fallimento di allocazione, zero allocazioni vive. Il bridge resta inerte finché f3-50 non lega CiukiDOS al VMM. Aperti e assegnati alla continuazione f3-49b (emendamento 1): cancellazione del trasporto (H49-08) sul pool di richieste f1-83, vita del DTA copiato, matrice dei guasti per mutazione, replay dell'adattatore dal vivo, budget dei cicli su cache.
3. **Riconciliazione del VFS.** La fusione di f1-83 su main con f3-49 ha prodotto conflitti semantici in `src/kernel/fs/vfs.c`/`vfs.h` (richieste cancellabili con ticket per unità vs separazione lettori/mutazioni). Risolti da Codex in un worktree di fusione dedicato (`build/codex/f1-83-merge-report.md`): entrambe le semantiche conservate, callers verificati senza modifiche, nuovo test `tests/host/fs/test_vfs_admission.c`; dopo la fusione latenza massima del lettore 9,838 ms, zero scadenze mancate, H49 invariato. Suite fs e kernel verdi su main `9d85d7eb`.
4. **Altre fusioni della serata.** f5-08 (`f8775e11`), f5-05b (`c8efd4d7`), f5-09 (`92591826`), f5-07A (`7cebeba2`): si veda l'indice delle direttive.

## Decisioni

1. Una consegna parziale si fonde quando le suite host sono verdi e il codice è inerte o non regressivo in produzione; la parte mancante diventa un emendamento con sessione di continuazione, mai un rilassamento dei criteri (f3-49 → f3-49b).
2. I conflitti semantici del kernel fra due consegne si risolvono in un worktree di fusione da Codex con le stesse suite di entrambe le consegne, poi il lead esegue il commit; la risoluzione manuale del lead resta per i conflitti meccanici.
3. Sequenza successiva: f1-84 (C2/C5) e f3-49b lanciate; poi f1-85 → f0-47 → f0-48 → f0-49 → f3-46; f3-50 in corso → f3-51.

## Riferimenti

- `docs/directives/f1-83-*.md`, `f3-49-*.md` (emendamento 1); `build/codex/f1-83-report.md`, `f1-83-merge-report.md`, `f3-49-report.md`; voci 2026-10-19-04, -06, -07, -08.
