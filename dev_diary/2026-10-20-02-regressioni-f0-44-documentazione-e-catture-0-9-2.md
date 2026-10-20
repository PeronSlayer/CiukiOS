# Prima immagine integrata dopo la sospensione dei gate: regressioni di f0-44 trovate per bisezione e corrette, build canonico riparato, documentazione e catture della 0.9.2

- **Data:** 2026-10-20
- **Tipo:** integrazione, regressione, documentazione
- **Versione/commit:** 0.9.2 pre-alpha / main `cc81cbb6`; immagine QEMU `116ced5d7c549a2fbb77c0e16282aa06e8de23b78623175bc2774d01e290a624`
- **Stato:** due regressioni corrette e verificate su QEMU; una regressione delle VM DOS aperta (bisezione in corso)

## Fatti

1. **Build canonico.** Il primo `make build-full` dopo le fusioni del 19 ottobre falliva in tre punti, comparsi da quando il riferimento FreeDOS è installato (`third_party/freedos/1.3/UPSTREAM.json`): il disco dei giochi non registrava le directory annidate dei payload DOS (`/SYSTEM`, `/D48`), il file d'identità del riferimento aveva un nome non 8.3 (`REF.JSON` → `REF.JSN`), il volume di riferimento era tagliato a metà dell'ultimo cluster e la rilettura usciva dall'immagine. Correzioni del lead `8f3bcda9`, `8f86c3ad`; test del disco dei giochi e d'integrazione DOS verdi.
2. **Test del pager.** La consegna f0-49 aveva eseguito le suite con `detect_leaks=0`; su main `pager_test` falliva per 721.024 byte dello spazio d'indirizzi abbandonato di proposito al taglio di corrente simulato. Finestra LeakSanitizer limitata a quello stato (`d93726d7`).
3. **Regressione delle finestre e dello sfondo.** Sull'immagine 0.9.2 i programmi nativi non aprivano finestre (`Files: desktop connection failed: Broken pipe`) e il desktop non mostrava lo sfondo. Bisezione su QEMU (TCG pentium3, apertura di Files dal menu, controllo dei pixel di sfondo e barra): buona `3357582f`, buona `1f49d5bb`, buona `8ae026a6` (fusione f0-45), cattiva `4d78e724` (fusione f0-44), cattive `179b7b56` e `0ac968f2`; tra `8ae026a6` e `4d78e724` solo commit di documentazione.
4. **Causa.** Da f0-44 ogni scorrimento della lista delle aree di un processo tiene un pin di mutazione su tutto lo spazio d'indirizzi e cede la CPU ai punti di servizio; un altro thread dello stesso processo riceveva EBUSY da mmap, pin dei buffer e copie verso l'utente (`read`/`stat` copiano sotto il lock del file system, dove l'attesa è rifiutata). Il desktop, multithread, perdeva i client e il lavoratore dello sfondo falliva con "Device or resource busy (16)".
5. **Correzioni.** `dbb0d714`: il pin di metadati di un altro task si attende (come già il percorso dei page fault), il pin del task stesso resta un rifiuto (test di f0-44 invariato); `cc81cbb6` (diagnosi di un agente Claude Opus): una copia coperta dal pin già preparato dal chiamante non prende un secondo pin; test di regressione `test_peer_walk_copy`. Suite kernel, fs, apps, desktop (122), Win32 (184) e gate (11) verdi; su QEMU Files, Pannello di controllo e sfondo funzionano.
6. **Regressione aperta.** Sulla stessa immagine un gioco DOS avviato da Programmi > Giochi resta a "Loading DOOM... Preparing..." (record `[launch-stage] name=dosbox stage=vm-create`, il processo dosbox consuma CPU nel kernel per oltre 4 minuti). Bisezione automatica `git bisect run --first-parent` tra `3357582f` e `cc81cbb6`, con le due correzioni applicate a ogni passo.
7. **Altre fusioni.** f5-10C (dialoghi, controlli classici, menu, MessageBox, font TrueType; conflitti risolti dal lead, integrazione dei test da un agente) e f5-20A (184 esportazioni di kernel32, ntdll, advapi32, version per le DLL di Wine); emendamenti nelle direttive.
8. **Documentazione e catture.** README, Roadmap, CHANGELOG (sezione 0.9.2), indice di `docs/`, `build-and-run`, `boot`, `desktop` aggiornati al 20 ottobre; undici catture QEMU in `docs/screenshots/0.9.2/` (desktop, menu, Informazioni con 0.9.2, Files, proprietà dello schermo, CiukNote, Task Manager, Music Player, CiukPaint, Terminal, menu Giochi) dall'immagine `116ced5d`.

## Decisioni

1. Ogni fusione che tocca kernel o desktop richiede di nuovo il build canonico; un avvio QEMU breve del desktop (apertura di un programma, sfondo) precede ogni immagine destinata all'hardware, anche con i gate sospesi.
2. Gli agenti eseguono le suite con le opzioni dei sanitizer del repository; una consegna che disattiva il rilevamento dei leak non si accetta senza motivazione.

## Riferimenti

- `docs/directives/f0-44-*.md`, `f0-49-*.md`, `f5-10c-*.md`, `f5-20-*.md`; `docs/screenshots/0.9.2/README.md`; voci 2026-10-19-03, 2026-10-20-01.
