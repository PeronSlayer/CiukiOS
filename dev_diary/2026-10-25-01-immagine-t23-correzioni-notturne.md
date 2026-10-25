# Immagine T23 2287114d…: blocco allo splash, chiusura delle finestre DOS, memoria DOS, spooler, NetSurf e revisione grafica

- **Data:** 2026-10-25
- **Tipo:** modifica, integrazione, decisione
- **Versione/commit:** main `b60e5c36`; immagine `2287114dfe8207706310983744f9d24c549093aafd1453d2cfe798aee4a10a66`, immagine hardware `9bb19145ca6c5ad4cd2f23ec28ab987f41c72e3fdb18b28a7f29141e07b11414`, CD di Setup `f8db72a0ef067e23c90642f5551e95430f8b7fc5ed3b0589e120890df6f3c400`
- **Stato:** completato su host e QEMU; prova T23 da fare

## Contesto

L'immagine del 24 ottobre (`56e58374`) si fermava sullo splash del T23. In
QEMU arrivava al desktop, ma molte applicazioni non funzionavano, la grafica
di più applicazioni andava rivista, mancavano le icone per programma e i
giochi DOS non partivano.

## Cosa cambia

Cause trovate e correzioni (ogni ramo integrato con controllo delle righe
perse, `build/lead-scratch/lost-lines.py`):

- **Blocco allo splash del T23.** `cache_publish` usava un try-lock e poi
  un'attesa di prenotazione che prendeva e rilasciava il ticket FIFO di
  `fs_lock`. Il task del marker video (Savage) e lo scrittore dello storage
  si rincorrevano circa 20.000 volte al secondo e il desktop non partiva.
  Ora la pubblicazione usa la presa FIFO semplice, come `cache_peek`
  (`src/kernel/fs/cache.c`; `docs/validation/t23-boot-hang-2026-10-24.md`).
- **Blocco totale alla chiusura di una finestra DOS** (DOOM con Alt+F4,
  oppure `exit` al prompt). Il servizio CD audio (`vm-cdaudio`) considerava
  "lavoro pendente" uno slot in chiusura finché `vm-cdread` non rilasciava il
  file, quindi la sua attesa tornava subito e il task girava senza cedere la
  CPU. Si fermavano il lettore, il desktop e il log seriale. Diagnosi con i
  backtrace del kernel presi via gdbstub di QEMU. Ora uno slot in chiusura è
  pendente solo dopo il rilascio del lettore (`src/kernel/vm/dosvm_cdaudio.c`).
  Il nuovo test 6a di `tests/host/vm/cdaudio_test.c` fallisce sul codice
  precedente.
- **`exit` nella finestra DOS.** Il guest avviava FreeCOM come shell primaria
  permanente (`SHELL=... /P=C:\FDAUTO.BAT`), che ignora EXIT. Ora avvia
  `CIUKISH.COM` (`src/com/dossh.asm`): esegue `\COMMAND.COM /K C:\FDAUTO.BAT`
  in modo non permanente e all'uscita chiede al VMM l'uscita pulita (porta
  0EAh). La configurazione di riferimento della batteria di conformità
  mantiene `/P`.
- **Spazio libero nel DIR del DOS** (circa 800 GB su un volume da 512 MiB).
  La risposta INT 2Fh/1112h sub 0Ch del drive rediretto metteva il media ID
  in AH, e il kernel K2043 lo copia senza maschera nella dimensione del
  cluster di 7303h. Ora AX porta solo i settori per cluster. AH=36h resta
  limitato a 2 GB - 32 KB come su Windows 98 FAT32.
- **Memoria DOS e Wolfenstein 3-D.** Il kernel K2043 non inoltra
  INT 2Fh AH=43h, quindi i programmi non vedevano XMS e FreeCOM restava
  residente (68 KB). Ora il VMM risponde al controllo d'installazione XMS
  prima del DOS, come HIMEM.SYS sotto Windows 98. Il blocco libero al prompt
  passa da circa 507 KB a 575 KB. Wolfenstein 3-D (soglia 515 KB, calcolata
  dal sorgente id Software) parte. Un programma che termina nei primi 10 s
  lascia la finestra con il suo testo e il titolo "Finished - <nome>".
- **Spooler di stampa.** Il passaggio inattivo non dormiva mai:
  `nanosleep` con `tv_nsec=1000000000` è EINVAL. Prendeva il 65–77 % della
  CPU e teneva C: per circa 112 ms a lettura. Ora è a eventi, come lo spooler
  di Windows: una scansione all'avvio, campanello da libprint, dalla cattura
  LPT del DOS e dalla pubblicazione Win32, ritentativo di 15 s per i lavori
  offline, riscansione di sicurezza ogni 60 s. Nella prova finale lo spooler
  usa circa lo 0,3 % della CPU.
- **Audio.** Dopo uno stop per inattività il trasporto mantiene per 30 s il
  margine cresciuto, poi lo riduce. Un underrun fa crescere il margine.
  Effetto: un suono che parte subito dopo un altro non inizia con un
  underrun. I "buchi" nel WAV di QEMU all'avvio di DOOM erano suoni separati
  dell'interfaccia con le loro code di silenzio
  (`docs/validation/doom-launch-audio-2026-10-24.md`).
- **Verifica dell'immagine DOS e registro di avvio.** La verifica all'avvio
  blocca la versione VFS dell'immagine, così scritture estranee su C: non la
  invalidano. Il registro degli avvii (`CIUKIST.LOG`) passa da exec a un
  task dedicato. Dopo la revisione, lo spegnimento scrive prima il suo
  marcatore e rifiuta con EBUSY invece di attendere uno scaricamento in corso.
- **NetSurf.**
  - **Immagini.** Il gestore PNG/JPEG di G26 sostituiva quelli upstream,
    saltando la decodifica in scala di fx-24 e fx-24b: le immagini oltre
    1 megapixel non si vedevano.
  - **JavaScript.** La sua inizializzazione andava in abort o in overflow
    dello stack quando il budget di memoria rifiutava un'allocazione.
  - **Appunti.** Il collegamento agli appunti di sistema di G5, perso in un
    merge di integrazione, è ripristinato in libdeskui.
  - **Chiusura.** All'uscita il processo non smonta l'heap JavaScript (la
    memoria la recupera il sistema). Prima lo smontaggio esauriva lo stack
    di 1 MiB in `duk__mark_heaphdr`.
  - **Barra degli indirizzi.** Ctrl+L inseriva anche la "l" nell'indirizzo
    (`lhttp://`). Per questo le pagine remote risultavano "Unable to open
    this address".
- **Grafica e icone.** Icone per programma. Revisione visiva delle schermate
  del collaudo, divisa in gruppi:
  - finestre Apri/Salva con, visualizzatore immagini;
  - Pannello di controllo: trackbar, griglia di icone, schede senza
    annidamento;
  - Musica, WordPad, Paint (palette a 28 colori), carte, Orologio;
  - barra e Proprietà della finestra DOS;
  - Gestione attività, Proprietà del disco, CiukIDE, Informazioni di sistema,
    Visualizzatore log, finestra Arresta il sistema con scelte a radio;
  - menu Start sotto il pulsante e icone cartella per i gruppi;
  - indicatore della lingua (riquadro blu con "En"/"It");
  - icona di alimentazione nascosta senza batteria;
  - "CiukiOS" al posto di "Windows" negli avvisi.
- **CD di Setup.** Il componente di sistema include `/System/SFC.LST` e
  `/System/printspool`.

## Decisioni

- **JavaScript disattivato di default** in NetSurf sul target. È una
  decisione temporanea: eseguendo lo script di example.com il motore Duktape
  legge da un oggetto nullo (`duk_hobject_find_entry_tval_ptr_stridx`,
  indirizzo 0x200). Si riattiva dalle opzioni di NetSurf. Registrato in
  `docs/design/netsurf-port-plan.md`.
- **Spooler a eventi** con riscansione di sicurezza a 60 s.
- **Margine audio mantenuto per 30 s** dopo uno stop.
- **Finestra DOS che resta aperta** per un programma che termina entro 10 s.

## Verifica

- **Suite host sul codice integrato** (`1d84c3f2`…`b60e5c36`):
  - kernel, filesystem, Win32, NDIS, USB, rete, audio e lwIP verdi;
  - desktop 223 test verdi, font verdi;
  - suite generale 1458 test: i tre fallimenti (due harness di storage e il
    test dello stack del kernel, che richiede il kernel compilato) sono
    corretti o verificati su main;
  - NetSurf 29 test verdi; appunti e WordPad 36 test verdi.
- **Build canonica su main** `b60e5c36`: T1 superato, CD di Setup costruito.
- **Smoke QEMU:** `boot`, `normal-boot-qemu-t23` e
  `normal-boot-qemu-desktop-2002` superati.
- **Collaudo QEMU (KVM) sulla stessa immagine:**
  - tutti i 37 programmi si aprono (prova sull'immagine `9562c910`);
  - calcolatrice, salvataggio in CiukNote e Terminal (dir, ipconfig);
  - DIR del DOS con lo spazio libero corretto; `exit` chiude la finestra;
  - DOOM, Wolfenstein 3-D e Quake partono dal menu Giochi, si giocano e si
    chiudono senza blocchi;
  - NetSurf apre http://example.com (200, 577 byte), mostra la pagina
    d'errore per un server che rifiuta la connessione e si chiude senza
    crash;
  - nessun `[fault]` né panic nei log seriali.
- **DOOM timedemo demo3 in QEMU:** 2134 gametic in 969 realtic (circa
  77 fps; misura del costo della VM su questo host, non del T23).
- **Non provato:** il T23.

## Riferimenti

- `docs/validation/t23-boot-hang-2026-10-24.md`
- `docs/validation/doom-launch-audio-2026-10-24.md`
- `docs/design/dos-dpmi-contract.md` (XMS davanti alla catena INT 2Fh,
  versione bloccata)
- `docs/design/printing.md`, `docs/design/f4-audio-media-contract.md`
- `docs/design/netsurf-port-plan.md`, `docs/design/desktop-shell.md`,
  `docs/design/common-dialogs.md`, `docs/design/dos-program-properties.md`,
  `docs/design/disk-maintenance.md`
- id Software, sorgente di Wolfenstein 3-D (`WL_MAIN.C`, `ID_MM.C`);
  Ralf Brown's Interrupt List (INT 21h/36h, 7303h)
