# F0: kernel Ciuki VMM, loader e runner avviati su QEMU

- **Data:** 2026-10-09
- **Tipo:** modifica (codice F0, sostituzione atomica del build)
- **Versione/commit:** commit di questa voce
- **Stato:** completato su QEMU (55/56 casi di f0-core, sonde 10/10); T4 (hardware) e safe-mode automatico aperti

## Contesto

Dopo i sette contratti ([2026-10-09-07](2026-10-09-07-contratti-f0.md)) e i
profili gemelli ([2026-10-09-08](2026-10-09-08-gemelli-t23-e500.md)) è
iniziato il codice F0, diviso come previsto: Claude il kernel e
l'integrazione, Codex il loader e il runner in due worktree separati.

## Cosa è stato fatto

- **Kernel** (`src/kernel/`, C + NASM, clang/ld.lld/NASM bloccati in
  `config/toolchain.json`): entry con paginazione a 0xC0000000, GDT/TSS
  (più TSS per il double fault), IDT con gate d'interrupt DPL 3 per le
  syscall, PIC con EOI gestito solo dal dispatcher e IRQ7/15 spuri, PIT a
  1000 Hz, allocatore fisico a zone con prenotazioni, mappa diretta e
  tabelle del kernel condivise (PDE 768–1022, 1023 ricorsiva), stack del
  kernel con pagina di guardia, spazi di indirizzamento utente, heap,
  scheduler a priorità fisse con preemption dei task a ring 3, FPU lazy via
  `#NM`, le sei syscall F0 congelate, console seriale e grafica (font
  Inconsolata OFL dal progetto), panic senza allocazioni, registro delle
  risorse con inventario PCI in sola lettura, misura TSC delle sezioni
  critiche, e le dieci sonde di `f0-acceptance.md` con i loro payload a
  ring 3.
- **Loader e immagine** (Codex): MBR, `CIUKLDR` da 45 settori con fw_cfg,
  politica input E500, A20, E820 normalizzata, FAT32 in sola lettura, menu,
  selezione VBE, caricamento ELF32; `scripts/build_image.py` con i controlli
  T1 e ora il `build-manifest.json`.
- **Runner** (Codex): `scripts/test/run.py` con lock comune ai worktree,
  scope systemd verificato, overlay qcow2, QMP, parser dei record,
  `result.json`; profili `qemu-t23/e500/min128/fast`; suite `f0-smoke`,
  `f0-core`, `f0-panic`, `f0-runner`; fixture di replay T23/E500; modello
  di riferimento del loader; 30 test host.
- **Makefile** della transizione: `build-full` = kernel + immagine F0,
  `qemu-test-full` = `f0-smoke`; i target 0.8 restano come `legacy-*`.

## Verifica (QEMU TCG, profilo qemu-t23, 512 MiB)

Dettagli in `docs/validation/2026-10-09-f0/README.md`.

- Primo avvio: loader e kernel partono insieme al primo tentativo. Due bug
  del loader (operandi con `BP` che usano il segmento SS, con SS=0):
  selettore sempre malformato e tutti i modi VBE scartati. Corretti con
  prefissi `ds:`.
- Tutte le dieci sonde del kernel passano in un solo avvio `f0:all`. Due
  correzioni ai test (non al kernel): lunghezza del caso "wrapping" e
  indirizzo del panic, che a 512 MiB cadeva nella mappa diretta.
- **Deviazione di QEMU TCG:** `FLD m64` viene arrotondato alla precisione
  della control word x87 (sull'hardware reale no). La sonda FPU usa ora
  control word a 64 bit di precisione; annotato in AGENTS.md.
- Runner: `f0-smoke` PASS; `f0-panic` PASS sul caso seriale, il caso "UART
  assente" ha prodotto lo screenshot atteso ma per contratto richiede
  conferma dell'operatore; `f0-core` prima esecuzione: 5 avvii a freddo
  PASS, riavvio a caldo fallito per un timeout di 30 s nella suite invece
  dei 180 s del contratto (corretto). Seconda esecuzione: vedi sotto.

- **Revisione incrociata di Codex sul kernel** prima del commit: 13
  problemi bloccanti e 2 minori, tutti applicati (log con interrupt
  disabilitati, CR4 non azzerato, immagine FPU iniziale, alias del
  framebuffer, validazione delle maschere, perdita di pagine in `spawn`,
  iniezione di record via `probe_report`, record troncati, misura delle
  sezioni critiche, contatore di preemption saturato, controllo ST1 vacuo,
  canali DMA, boost anti-starvation, classificatore dell'audit, UB sulla
  negazione). Tabella completa nel record di validazione.
- **Riavvio a caldo:** con `-no-reboot` QEMU trasforma il `system_reset`
  dell'host in uno shutdown (prova nel log QMP); il runner ora omette
  `-no-reboot` solo nei casi di restart.
- **Secondo giro di revisione:** Codex ha confermato 6 correzioni, chiesto
  di più su 9 e trovato una regressione (pagina di guardia MMIO oltre la
  fine della regione). Applicate le correzioni concrete (CR4 anche per i
  bit SSE, framebuffer non sovrapposto a primo MiB e kernel, capacità del
  componente riservato, limiti MMIO con guardia, `owner_at_kill` nel PASS
  FPU, boost alla priorità massima, classificatore per `movss`/`cmpsd`,
  conteggio esatto dei reset dell'host nel runner). Verifica mirata:
  APPROVE.
- **Riavvio a caldo, seconda causa:** dopo il `system_reset` dell'host
  SeaBIOS esegue un proprio reset (`RESET reason=guest-reset`); il runner
  ora tollera un solo reset del firmware entro 3 s da ogni reset dell'host
  e richiede il conteggio esatto di quelli dell'host. Caso di riavvio
  verificato singolarmente: PASS.
- **Decisioni di Claude in disaccordo con Codex**, approvate dal
  proprietario il 9 ottobre insieme alla conferma dello screenshot del
  panic senza UART (run `69573015`):
  - i gap del timer senza causa software sono *riportati* come non
    classificati e non fanno fallire la sonda: dall'interno della VM non si
    può dimostrare chi ha fermato la macchina (un'attribuzione all'host via
    SMBIOS era stata provata e scartata); la risoluzione spetta alla
    qualificazione, con il carico dell'host ora registrato dal runner;
  - sotto TCG il TSC conta anche il tempo sottratto dall'host: un run
    eseguito con test host e revisione Codex in parallelo ha misurato
    `critical_us=442` contro 35–66 µs a host libero. Le misure di latenza
    vanno prese con l'host libero; la condizione `budget_violations=0`
    resta come da contratto;
  - validazione grammaticale completa di `probe_report` rimandata a F2;
    rimandati anche i test con iniezione di guasti in `spawn`, l'RDTSC
    serializzato e il log per singolo boost.

- **Tempo del guest sotto TCG:** la suite sull'immagine corretta ha
  superato tutto il blocco T23 (10 avvii con riavvii a caldo, 9 sonde) e i
  10 avvii E500, poi `preempt` sul profilo E500 ha misurato 976 µs con host
  libero: sotto TCG il TSC è tempo reale e include la traduzione del codice
  da parte di QEMU. I profili di prova usano ora `-icount shift=1,sleep=on`
  (tempo sintetico: 2 ns per istruzione durante l'esecuzione, idle
  allineato al tempo reale): la stessa sonda riporta `critical_us=0` (sotto
  1 µs; ora c'è anche `critical_ns`) e nessun gap. Precisazioni di Codex
  accolte: l'icount nasconde gli stalli dell'host, non li misura; il budget
  di 250 µs sotto emulazione è una soglia di regressione in
  tempo-istruzioni (~125.000 istruzioni), non la prova del requisito
  fisico, che spetta ai portatili. Registrato in `f0-acceptance.md`.

**Esito delle suite sull'immagine finale (profili icount, immagine
`5349514c…c8ac6`):** `f0-smoke` 2/2 PASS; `f0-panic` PASS (il caso senza
UART è confermato dal proprietario dallo screenshot); `f0-core` 55/56 PASS:
30 avvii (a freddo e con riavvio a caldo) sui profili T23, E500 e 128 MiB,
le otto sonde non distruttive su ogni profilo, il fallback video. L'unico
caso non superato è `safe-mode`, che il runner dichiara non automatizzabile
con il selettore congelato: resta aperto (serve una chiave `safe=1` da
fw_cfg nei contratti e nel loader).

## Aperto

- T4: scrittura dell'immagine su un disco sacrificabile e prove su T23 ed
  E500 con evidenza a schermo.
- Build dell'immagine deterministici.
