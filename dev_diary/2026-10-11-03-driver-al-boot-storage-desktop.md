# Driver avviati al boot, storage FAT montato, oggetti del desktop, suite F1 allineate

- **Data:** 2026-10-11
- **Tipo:** modifica
- **Versione/commit:** da `7f3d906` (f1-08) a `5a654a7` (f2-05)
- **Stato:** integrato su `main`; F0 56/56 sull'immagine `acd8af37…`; suite F1/F2 su QEMU in corso

## Cosa è stato fatto

Implementazioni di Codex (Sol e Astra), fusioni e collaudi del lead.

- **f1-08** (Sol): `drivers_init` dopo la calibrazione: framebuffer, backend
  di input selezionato dalla politica di avvio (i8042 nativo o BIOS VM via
  `fwinput_adapter`), ATA; modalità provvisoria con sole console e input;
  riserve di avvio pubblicate come lease firmware; decoder nativo tradotto
  ai codici set 1 pubblici; scadenza risposte 200 ms; sonde `registry` e
  `safe`; `-fstack-protector-strong` con guardia globale seminata in
  `registry_init`.
- **Regressione e correzione** (f1-11, Sol): la prima immagine con i driver
  al boot (`0ba29b3e…`) emetteva record di attivazione prima del `BEGIN` e
  `f0-smoke` falliva; un avvio manuale ha mostrato driver pronti
  (framebuffer, input nativo, ATA identificato). Ora `drivers_init` scrive
  un ledger e la sonda `boot` emette i record dopo il `BEGIN`.
- **Evidenza F0** sull'immagine `acd8af37…` (commit `b6c1562`): `f0-smoke`
  2/2, `f0-core` 56/56, `f0-panic` come da record F0: primo gate F0
  superato con i driver F1 attivi. Record:
  `docs/validation/2026-10-11-integrated-image-f0-regression.md`.
- **f1-10** (Sol): predicati delle suite F1 allineati ai record reali delle
  sonde; i casi a conferma dell'operatore non bloccano più i casi seguenti;
  selettore e dispatch spostati in `selector.c` con test dal mappa del
  linker; la scelta Normal del menu non annulla più la modalità provvisoria
  richiesta da `BOOT.CFG`/fw_cfg; arbitraggio righe console/presenter.
- **f1-09** (Astra): montaggio FAT all'avvio (solo lettura, poi scrittura
  dopo il gate di lettura e la verifica di durabilità), writer periodico sul
  worker, sonde `fat-read`/`fat-write`/`cache`/`mount-crash`/`bootlog`,
  boot log limitato aperto solo dopo la qualificazione, provider RTC in sola
  lettura (5.683 controlli); 39.944 controlli di storage; hook di cattura
  dell'output applicato all'integrazione.
- **f2-05** (Astra, parziale): superfici con backing a riferimenti, canali
  limitati con fd di superficie allegati, grant di display e input installati
  solo dal supervisore, syscall 57–64 e 67, sonda `crash-isolation` con
  server di prova e controller `libc-smoke`; patch d'integrazione (mappature
  condivise in `uaddr`, hook del supervisore, payload) applicata alla
  fusione; 35.965 controlli.
- Direttiva **f1-12** emessa per i residui: percorso e tetto del boot log
  secondo contratto (`\SYSTEM\LOGS\BOOT.LOG`, 128 KiB), ordine degli oggetti
  delle sonde, policy di overflow della coda input lato produttore
  (RESYNC con perdita e stato pulsanti, ripetizioni con `value=2`),
  verifica degli hook del supervisore.

## Decisioni

- Binding del disco di avvio: per F1 `C:` è il disco ATA 0, partizione
  primaria 1, con `qualified=0 reason=loader_fingerprints_absent`; il
  confronto con le impronte del loader diventa obbligatorio con
  `ciuki_boot_info` v2 (emendamento in `f1-acceptance.md`).
- Il kernel ora avvia i driver anche in un boot ordinario (senza selettore);
  la modalità provvisoria limita le attivazioni a console e input.
