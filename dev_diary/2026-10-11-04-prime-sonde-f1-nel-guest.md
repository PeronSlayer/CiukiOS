# Prime sonde F1 superate nel guest; file POSIX, desktop e correzioni

- **Data:** 2026-10-11
- **Tipo:** modifica
- **Versione/commit:** da `a8e4c8a` (f2-03) a `5f45688` (f1-15)
- **Stato:** integrato su `main`; batch completo F1/F2 su QEMU in corso

## Cosa è stato fatto

- **Fusioni** (tutte implementazioni di Codex): f1-14 (fixture di
  `input-fault` in memoria statica: l'allocazione di 10.980 byte superava il
  limite di 2.040 byte del kmalloc e falliva solo nel guest), f2-03 (tabella
  fd, vista POSIX dei percorsi, syscall file 29–49, `/dev/null` e
  `/dev/console`, orologi con REALTIME seminato dal provider RTC; 14.837
  controlli host; patch d'integrazione applicata), f2-08 (desktop a ring 3:
  compositor, protocollo su canali, routing dell'input, client demo con i
  guasti dell'accettazione; ritratto di Ciuki convertito in modo
  deterministico a 256×256 e verificato a vista dal lead), f1-16 (fixture
  FAT12/16 "superfloppy" montate come volume intero; numerazione unica dei
  dischi fixture fra runner e sonde), f1-15 (il backend input firmware su
  QEMU falliva perché SeaBIOS legge il timer ACPI PM alla porta 0x608 e la
  policy delle porte lo rifiutava: timer PM virtuale solo su QEMU, trasferimento
  delle lease pubblicate, diagnostica su ogni percorso di errore).
- **Evidenza QEMU** (`docs/validation/2026-10-11-integrated-image-f0-regression.md`,
  immagini sesta–ottava): prime sonde F1 superate nel guest — `registry`,
  `safe` sul profilo nativo, `ata`, `partition`, `ata-fault`, `input-fault`
  e `input` con lo stimolo QMP del contratto sul profilo T23. Falliti e
  corretti: `safe` sul profilo E500 (f1-15), `fat12-read` (f1-16); `f1-fat32`
  e `f1-storage` hanno predicati ancora da allineare ai record delle sonde
  FAT (direttiva f1-17 in corso). Le sonde F2 non hanno ancora girato nel
  guest: le suite F2 ripetono il prefisso F1 e si fermavano al caso E500.
- **Regole imparate e registrate**: nessuna fusione su `main` durante un
  batch (il passo preliminare del runner legge l'albero vivo); controllare
  esplicitamente la riga `OK` dei test host dopo ogni fusione; i
  `summary.json` di un batch abortito restano vecchi e non sono evidenza;
  le direttive devono elencare anche i file "di cucitura" (dispatch delle
  syscall, hook del build, `output.c`), altrimenti Codex consegna patch
  separate che il lead applica.
