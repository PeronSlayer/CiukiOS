# Prova del T23 sull'immagine e8dfd0cd, cause misurate (PIC, chiavetta, Quake) e immagine a79f840c

- **Data:** 2026-10-16
- **Tipo:** analisi (hardware reale), decisione (architettura DPMI, registrazione), modifica (kernel, driver, filesystem, VM DOS, desktop, runner), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `b193b7f0`; immagine QEMU `e763183c58da8ef47e2345190f3d0b0af27388b9c72c9837bbefdb76891adfe7`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `a79f840cf7960c72aa85771cc8fdfa94ee48edfac4493ff35adf25caf06f6707`
- **Stato:** immagine consegnata; verifica sul T23 in attesa

## Contesto

Prova del proprietario sull'immagine `e8dfd0cd` (voce 2026-10-16-01): miglioramenti evidenti; Quake e DOOM girano quasi giocabili con pochi fotogrammi; chiavetta USB ancora non rilevata; il resto a posto. Evidenze: cattura seriale pulita (`legacy/local/physical/e8dfd0cd/serial.log`, due avvii, righe intere, un solo lettore sulla porta) e log su disco `BOOT1.LOG` (avvio con pagina H) e `BOOT.LOG` (avvio dei giochi con diagnostica verbosa).

- **PIC.** `pic_mask writes=5048 out_max_us=2108`, `irqoff_section=pic_unmask … irqoff_max_us=2109`: una singola OUT al registro maschera dell'8259 costa 2,1 ms su questo ICH3-M/BIOS; circa 145 scritture al secondo a desktop fermo (driver ATA a ogni comando, i8042, catena), `Timer 10 s: ticks=9924 lost=76`.
- **Chiavetta (05dc:a81d, 16 GB, FAT32, 1 953 289 cluster).** Enumerazione e fasi BOT in 0,6 s; il montaggio legge 12 293 settori di FAT (circa 6 MB a ~330 KB/s su USB 1.1) in 19 s; l'ultimo comando di lettura termina con la fase dati ferma a 64 576 byte su 65 536 e senza CSW; ripristino in timeout e quarantena con EIO (identico al secondo avvio, dopo 53 letture). La pagina H concede 13 s alla voce USB.
- **Quake e DOOM (record `vm_perf_*`).** Compito VM all'84–87 % del tempo, codice ospite al 21–38 %, monitor 2,3–3,3 s ogni 5 s: trap di debug 2,0–2,2 s (420–455 mila uscite, 90 000/s, 4,9 µs ciascuna: i passi singoli del tracciatore dei ritorni DPMI di f3-10); porte PIT/PIC 0,4–0,9 s (8 µs per uscita, fino a 23 000/s); INT in modo reale 1,0–1,5 s in caricamento (16–25 µs); `pm_flags` 80 µs per uscita.
- **Log su disco.** L'avvio dei giochi, con diagnostica verbosa, ha scartato 24 212 byte di record (anello RAM da 64 KiB, scarico a 32 KiB/s): perse le righe `vm_perf_*` sempre attive, sopravvissute solo le famiglie verbose di fine VM e 592 righe periodiche `[usb]`.
- Pagina H: Savage configurazione 1 PASS e configurazione 0 FAIL (conferma della scelta), seriale 100/100, voce "USB mouse" FAIL con 160 eventi ricevuti e nessun cambiamento riconosciuto (criterio della voce, mouse funzionante).

## Decisioni

- Nessuna scrittura della maschera PIC a tempo di esecuzione: i dispositivi gestiscono le proprie interruzioni (nIEN ATA, byte di comando i8042, prova di arresto SB16); le maschere restano per quarantena, differimento della catena e pagina H.
- I volumi rimovibili si montano subito in sola lettura con nove letture da un settore (BPB, copia di riserva, FSInfo, voci riservate delle FAT); la scansione di proprietà avviene al primo aggiornamento in scrittura (percorso f0-25), come fanno i driver FAT dei sistemi di riferimento.
- Il tracciatore dei ritorni DPMI non esegue più il passo singolo: con l'autorizzazione del proprietario, revisione del contratto f3-10 (il tracciatore è un meccanismo di compatibilità della IF virtuale, non la barriera di isolamento, garantita da paginazione a CPL3, LDT/GDT, IOPL e TSS) e riprogettazione con breakpoint di esecuzione sparsi su uno slot DR libero e uno scanner limitato; ripieghi a passi limitati per le istruzioni non decodificabili e quando i quattro slot sono del client; scadenza delle previsioni non raggiunte (100 ms o 64 interruzioni).
- QEMU 11.1.1 TCG muore con SIGSEGV quando l'ospite programma breakpoint di esecuzione nei registri DR (tre core dump di `qemu-system-i386`, `wrapper_returncode=-11`) e confronta i breakpoint con EIP anziché con l'indirizzo lineare: sotto QEMU il tracciatore usa l'acquisizione a passi TF verificata, sull'hardware i breakpoint; la riga `vm_perf_tracker` (`armed`, `hit`, `segmented`, `expired`, `fallback`) nel log del T23 è l'unica evidenza del percorso con breakpoint.
- Record protetti e record scartabili nel log di avvio: le evidenze a frequenza limitata non vengono mai scartate finché esistono record verbosi; lo scarico segue il passo del disco.

## Cosa cambia

- **f0-33** (`b7f484ee`): maschere dell'8259 costanti dopo l'attivazione; ATA usa nIEN (impostato prima della pubblicazione del gestore, azzerato solo intorno ai comandi), i8042 il byte di comando e l'inibizione delle interfacce, SB16 la prova di arresto; fixture host contano zero scritture PIC per richiesta.
- **f1-74** (`59d1f7bd`): gara di completamento UHCI alla decisione di arresto (un TD completava tra la camminata del prefisso e la decisione: suffisso sganciato, trasferimento in timeout senza CSW), una ripetizione della READ(10) dopo reset di classe, montaggio immediato in sola lettura dei volumi rimovibili con scansione differita, voce USB della pagina H con tempo di montaggio e stato della scansione.
- **f0-34** (`5db1f16c`): classi di pubblicazione protetta e scartabile in 256 slot fissi, scarico ripetuto al passo del dispositivo, righe periodiche USB e SB16 solo con DIAG_VERBOSE; contratto `docs/design/boot-log-publication.md`.
- **f3-33** (`49a502a7`): dispatch diretto delle porte PIT/PIC, sequenza latch/lettura in un'uscita sola, INT/IRET in modo reale con un solo accesso al frame, prova dei flag PM con scarto anticipato (su host: riflessione 67 → 27 ns, decodifica dei flag 4745 → 1743 ns).
- **f3-34** (`8e36e952`, correzioni `0497bdb2`): tracciatore dei ritorni con breakpoint sparsi come sopra; replica host del timedemo di DOOM: uscite di debug dei ritorni 144 883 → 14 042, menu e suono 4 524 728 → 628 745.
- **f3-35** (`4d225f04`): selezione per piattaforma del tracciatore, righe `vm_start tracker=` e `vm_perf_tracker`, runner che riporta l'uscita anticipata di QEMU (segnale o codice) invece dell'errore di socket.
- **f2-77** (`9db48c30`): il desktop apre una volta sola `/dev/media`, `/dev/net` e l'osservatore del mixer (il confronto del registro dei processi di crash-isolation cadeva nella finestra apri/chiudi dei sondaggi, cicli 9, 28 e 33 su tre immagini); la sonda nomina la maniglia in eccesso.
- **f1-75** (`beb2d978`): la sonda di taglio corrente mette in pausa limitata lo scarico del log attorno alla sequenza di taglio (con f0-34 la scrittura del record ARM rubava il breakpoint blkdebug del runner).
- **f1-76** (`368a2d43`): firme di reset lette due volte per unità dopo l'attesa di BSY; un IDENTIFY abortito con firma PACKET passa a IDENTIFY PACKET (con due unità ATAPI sul canale secondario il master veniva classificato ATA e il canale finiva in quarantena; topologia solo di prova, il T23 ha una sola unità ottica).
- Direttive, fonti e decisioni: `docs/directives/f0-33…f0-34`, `f1-74…f1-76`, `f2-77`, `f3-33…f3-35`; `docs/validation/f0-30-irqoff-audit.md` (emendamento f0-33), `f2-77-desktop-poll-handles.md`, `2026-10-13-f3-10-menu.md` (emendamenti f3-34/f3-35), `docs/design/dos-dpmi-contract.md` (emendamento f3-34), `usb-contract.md` e `vfs-storage-contract.md` (emendamenti f1-74).

## Verifica

- Suite host dopo ogni fusione: kernel, filesystem, USB e archiviazione, desktop e gate, immagine DOS, build del kernel.
- Immagini intermedie: `64a531f9` (f0-33, f1-74): gate uguali alla notte; `27b35378` (f0-34, f3-33, f3-34 senza correzioni): dos4gw, dpmi e vm-fullscreen con QEMU morto per SIGSEGV (origine di f3-35), mount-crash-reboot con il breakpoint rubato (origine di f1-75).
- Catena finale: sdk, desktop 102/102, gate 11/11, apps, build-full con albero pulito (immagine QEMU `e763183c`, main `b193b7f0`), suite host del kernel, payload 9/9, immagine T23 `a79f840c`, f0-smoke 4/4.
- Gate QEMU sull'immagine finale `e763183c`: verdi fat32-read, cache, fat-write, mount-dirty, fd-table, libc-smoke, f1-usb 3/3 (usb-input-uhci, usb-input-ohci, usb-wheel-uhci: confermata f2-77), desktop-shell, f5-core 2/2, vm-window, f1-input 24/24, audio nativo 2/2, dos4gw e dpmi (QEMU resta vivo con il percorso TF di f3-35), vm-boot, vm-video, safe-remove-usb, optical-l1; rossi noti su TCG: quake (`exit error=-110`, scadenza della sonda, f3-30; QEMU vivo), vm-audio, vm-fullscreen (f3-29), rtl8139 `firmware_active`; aperti, di sola topologia di prova: mount-crash-reboot (anche con la pausa dello scarico di f1-75 una scrittura non tracciata precede i tre record `case=cut` e ruba il breakpoint blkdebug: da attribuire, la riparazione dopo taglio è verificata sul T23 dalla voce "Mount repair" della pagina H) e volume-resolution-cd-usb (con due unità ATAPI QEMU risponde all'IDENTIFY abortito con registri diversi da quelli attesi dal ripiego di f1-76: `[ata1] fault unit=0 command=ec … error=04`; il T23 ha una sola unità ottica).
- Sul T23: pagina H (voce USB con tempo di montaggio e scansione differita; timer senza tick persi; `pic_mask writes` fermo); chiavetta montata subito e scrivibile alla prima scrittura; log con `vm_start tracker=breakpoint` e `vm_perf_tracker hit>0`; Quake e DOOM con i record `vm_perf_cost` da confrontare con quelli di `e8dfd0cd`.

## Riferimenti

- `legacy/local/physical/e8dfd0cd/` (seriale e log su disco, non pubblicati); `build/codex/f0-33…f3-35-report.md` (rapporti degli implementatori, locali); `coredumpctl` del 16 ottobre (tre SIGSEGV di `qemu-system-i386`).
