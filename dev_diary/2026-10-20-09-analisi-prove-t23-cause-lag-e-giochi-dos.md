# Analisi delle due prove T23 fallite (3d18f2db, 5b74e640): finestra DOS che affama la propria VM, ridisegno continuo di Files, contesa sul volume C:

- **Data:** 2026-10-20
- **Tipo:** analisi hardware
- **Versione/commit:** 0.9.2 pre-alpha / immagini T23 `3d18f2db…` (main `588609e7`) e `5b74e640…` (main `928b893b`); confronto con QEMU TCG (profilo qemu-t23) sulla stessa immagine
- **Stato:** cause individuate; correzioni affidate alle direttive f3-52, f2-86, f1-86

## Fatti

1. **Sintomi uguali nelle due prove** (il proprietario: Files molto lento con tutto il sistema che rallenta, giochi DOS fermi alla pagina di caricamento), benché la seconda immagine contenga la correzione della coda ATA (`7c5b9878`): nella seconda i limiti della coda restano tra 5.632 e 65.536 byte, mai 512. La coda non era quindi la causa principale.
2. **Giochi DOS.** Sul T23, dopo l'hash del disco dei giochi (23,8 MB letti in circa 10 s, cioè 2,8 MB/s) e la lettura del WAD di DOOM (circa 5 MB), il task dell'**applicazione** DOS Box occupa il 50–70 % della CPU, tutto in tempo kernel (`[cpu] top=dosbox#35:700k700s7`, `stretch_max_ms=11 eip=kpreempt_point`), mentre il thread della VM `dosvm#37` riceve circa il 5 %; la CPU è satura (`idle_pm=8..51`). La VM non presenta mai un fotogramma. In QEMU la ripartizione è opposta (`dosvm#32` 58–64 %, `dosbox#30` 5–11 %) e DOOM arriva al titolo. L'applicazione, con la finestra a fuoco, chiama `CIUKI_DOSVM_PRESENT_BANDS` ogni 2 ms (circa 500 chiamate al secondo); ogni chiamata appunta la superficie del client, sveglia la VM se è in riposo ed esegue `dosvm_vga_present`; l'applicazione ha priorità di base 9 (10 con il boost di I/O), il thread della VM 7. La pagina di caricamento invia inoltre un danno a tutta la finestra ogni 80 ms; il pannello delle impostazioni ogni 20 ms e l'avviso di errore ogni 10 ms, pur essendo statici.
3. **Files.** Con Files aperto e inattivo il suo task usa il 50–65 % della CPU in spazio utente (16 % in QEMU). Nel toolkit `libdeskui`, `du_emit()` segna l'intera finestra da ridisegnare per ogni evento, compresi i tick dei timer: il timer da 20 ms di Files produce fino a 50 ridisegni completi e 50 confronti dell'intero buffer al secondo (finestra 760×520: circa 20 milioni di pixel azzerati e 160 MB/s confrontati) senza inviare danni. Il ciclo principale del desktop dorme 1 ms invece dei 10 ms dichiarati (circa 1.000 giri di servizio al secondo).
4. **Contesa sul volume C:.** 144 e 232 record `[storage] vfs_wait drive=C ms=100..1415 holder=C reason=volume`, concentrati con Files aperto e durante Quake; assenti nelle immagini fino al 2026-10-15 (`763eefa6`, `56d41c09`), 50 nell'immagine del 2026-10-18 (`287551c6`). Il record non dice quale task tiene la prenotazione né per quale mutazione.
5. **Altri dati comuni alle due prove**, assenti in QEMU: il test BCI della Savage fallisce (`init_result=-5`, driver in quarantena, composizione via software) come in tutte le immagini T23 registrate; sezioni a interrupt spenti di 2,1–2,4 ms in `schedule` (circa 800 per sessione, non attribuibili: il Pentium III non ha un contatore degli SMI; l'handoff UHCI azzera i bit di legacy support); `[work] budget exceeded site=timer_work us≈2110`; nessuna prova nel log che i trasferimenti ATA avvengano in DMA (`[ata0] dma mode=udma2` descrive solo la configurazione).
6. **Blocco del desktop nel caso `normal-boot-qemu-t23`** (profilo con `icount`): si riproduce anche sull'immagine `588609e7` di stamattina, quindi precede le fusioni di oggi; resta aperto con priorità inferiore.

## Decisioni

1. Correzioni con direttive separate: **f3-52** (attesa bloccante dei fotogrammi nel kernel al posto del polling a 2 ms, costo degli appunti di memoria, priorità del thread della VM mentre la sua finestra attende, pagina di caricamento a non più di 4 aggiornamenti al secondo), **f2-86** (i timer del toolkit non invalidano la finestra; ciclo del desktop a riposo da 1 a ≥10 ms; confronto prima di invalidare nel dialogo dei file), **f1-86** (strumentazione: task e operazione che tengono la prenotazione del volume, contatori DMA/PIO e tempi di servizio per comando, profilo delle uscite della VM sul seriale senza sonda, attribuzione delle sezioni lunghe a interrupt spenti).
2. La prossima immagine T23 si costruisce dopo la fusione di f3-52, f2-86 e f1-86 e una catena verde; la Savage in quarantena resta un tema di F7.

## Riferimenti

- `docs/directives/f3-52-*.md`, `f2-86-*.md`, `f1-86-*.md`; `docs/design/f7-3d-acceleration-contract.md` (Savage); log T23 in `legacy/local/physical/3d18f2db/` e `5b74e640/` (non pubblicati); voce 2026-10-20-07.
