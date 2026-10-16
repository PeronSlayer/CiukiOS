# Prova del T23 sull'immagine a79f840c: ottica, chiavetta all'avvio, uscita dallo schermo intero, log perso, SMI; immagine d598bf4c

- **Data:** 2026-10-17
- **Tipo:** analisi (hardware reale), modifica (kernel, driver, filesystem, desktop, VM DOS, pagina di autodiagnosi), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `9c34b985`; immagine QEMU `280e84c0d1f4afce5bb28bf3319afdbfe27dedef291abfc8b3dde47cd01f02cb`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `d598bf4ce5ac97c6587711f86e31176f0a83d120459ce329efca0d0e1fd3f283`
- **Stato:** immagine consegnata; verifica sul T23 in attesa

## Contesto

Prova del proprietario sull'immagine `a79f840c` (voce 2026-10-16-02), tre avvii con cattura seriale pulita (`legacy/local/physical/a79f840c/`, 751 KB) e log su disco: pagina H; menu L con chiavetta inserita prima dell'accensione (sistema caricato, periferiche "morte", poi blocco e spegnimento forzato); menu L senza chiavetta (tutto funzionante; Quake "migliorato di molto", schermo intero nativo "va benissimo", al ritorno il desktop resta congelato con i suoni attivi). Esiti misurati:

- Pagina H: chiavetta `PASS USB storage: drive=E mounted=1 mode=ro mount_ms=251 scan=deferred`; `Mount repair` PASS; timer ancora `lost=76` in 10 s con sezioni da 2,1 ms (`irqoff_section=schedule`) e `pic_mask writes=9`: lo stallo non dipende dalle scritture PIC; seriale 100/100.
- Unità ottica in quarantena in tutti gli avvii (`[ata1] fault unit=0 command=ec status=51 error=04`), mentre su `e8dfd0cd` la stessa unità era `kind=atapi` con firma `010114eb`: regressione di f1-76.
- Avvio con chiavetta: montaggio in 306 ms, poi `[desktop] stall stage=shell … wait=autoplay-service` per oltre 33 s con `no_frame_ms` crescente mentre i report del mouse arrivano (`hid latency … reports=34`): il servizio AutoPlay eseguiva sul thread dei fotogrammi la scansione del supporto e le letture delle preferenze. Il log di quell'avvio è andato perso: `BOOT1.LOG` di 0 byte dopo la rotazione, benché la scatola nera registri `bootlog writes=12 bytes=393216 metadata=0`: con il desktop fermo le pubblicazioni dei metadati tornavano EBUSY e la riparazione di f0-32 ha tagliato la coda non pubblicata (`repaired logs=1`).
- Schermo intero: ingresso corretto (`provider=supersavage-native`, primo fotogramma dall'apertura); all'uscita `native_supersavage phase=restore result=-5 stage=native-restore-streams reg=158 expected=00 actual=08`, `lease_return_ms=1180 budget_ms=200`, `mode=280 1024x768x32 result=-5`, poi il compositore produce fotogrammi ma `presents=0`.
- Quake a schermo intero (record `vm_perf_*` su disco): ospite 58–66 % del tempo (era 21–38 %), monitor 12–20 % (era 46–66 %), trap di debug 14–22 mila ogni 5 s (erano 420–455 mila), `vm_perf_tracker hits=22823 segmented=22823 fallback=0`: il tracciatore con breakpoint lavora sull'hardware. Il log di avvio non scarta più record protetti (`dropped_protected=0`, `dropped_verbose=42910`).

## Decisioni

- La classe ATAPI è decisa dalla firma LBA Mid/High 14/EB, indipendentemente dallo stato (00 oppure DRDY/DSC) e dai transitori di SC/LBA Low; un IDENTIFY abortito con quella firma ricade su IDENTIFY PACKET (ATA/ATAPI-6 r3a 7.15.6.2, 8.15.5.2, 9.12; QEMU `ide/core.c`; libata).
- Il servizio AutoPlay non esegue mai I/O sul thread dei fotogrammi: un lavoratore dedicato esplora il supporto e legge o scrive le preferenze; il servizio tratta un arrivo per giro, solo dopo l'abilitazione della shell e la prima presentazione.
- I file `BOOT*.LOG` recuperano la coda non pubblicata dalla prenotazione posseduta (byte stampabili, TAB/CR/LF, riempimento a zero), con ordine: ripristino del collegamento provato in entrambe le FAT, pubblicazione della lunghezza, poi liberazione; i file ordinari conservano il taglio di f0-32.
- SR58 (`reg=158`) è un risultato di bordo a sola lettura (riferimento registri S3 Savage4; X.Org non ne verifica l'uguaglianza): non si scrive né si confronta; SR59/5A idem; di SR5B si confronta il solo bit 0. Un ripristino nativo fallito non lascia mai il desktop senza scansione: il modo salvato viene ripristinato con il passaggio VBE mediato e il grant torna al desktop; il ritorno via watchdog usa la stessa transazione.
- Lo stallo periodico da 2,1 ms si misura prima di toccare il chipset: tracciato TSC delle sezioni sopra 1 ms nelle finestre del timer della pagina H, istantanea in sola lettura dei registri SMI/PM dell'ICH3-M, e una voce "SMI bit 10 s" che azzera un solo candidato documentato (LEGACY_USB_EN, SWSMI_TMR_EN o PERIODIC_EN) solo se l'evidenza lo qualifica, misura dieci secondi e lo ripristina; nessuna modifica negli avvii normali.

## Cosa cambia

- **f1-77** (`3a539de6`): classificazione ATAPI dalla firma, ripiego a IDENTIFY PACKET, riga di firma per unità (primo campione e campioni diversi).
- **f2-78** (`e468e966`): AutoPlay su lavoratore, macchina a stati sul thread principale, `autoplay_wait=` nell'osservatore degli stalli.
- **f3-36** (`3950c567`): tabella SR senza SR58–5A, prima fase di errore registrata, rilascio di alias e lease del provider anche in errore, ripiego VBE in `display_fullscreen_end`, stessa transazione per uscita pulita e watchdog.
- **f0-36** (`8a0d5d1f`): recupero della coda dei log (`[storage] log_repair file=… action=extended recovered_bytes=…`), contratto `vfs-storage-contract.md` emendato.
- **f0-35** (`18d50ce8`): tracciato degli stalli, istantanea SMI/PM, voce H "SMI bit 10 s".
- Direttive e fonti: `docs/directives/f1-77`, `f2-78`, `f3-36`, `f0-36`, `f0-35`; `docs/validation/f1-39-storage-throughput.md` (emendamento f1-77), `f2-78-autoplay-service.md`, `f3-32-native-fullscreen.md`, `f0-30-irqoff-audit.md` (emendamento f0-35). In corso: f3-37 (altre risoluzioni e profondità a schermo intero), nell'immagine successiva.

## Verifica

- Suite host dopo ogni fusione: kernel, filesystem, ATA, desktop 102/102, gate 11/11, immagine DOS, build del kernel.
- Catena finale: sdk, desktop 102/102, gate 11/11, apps, build-full con albero pulito (immagine QEMU `280e84c0`, main `9c34b985`), suite host del kernel, payload 9/9, immagine T23 `d598bf4c`, f0-smoke 4/4.
- Gate QEMU sull'immagine finale `280e84c0`: verdi fat32-read, cache, fat-write, mount-dirty, fd-table, libc-smoke, f1-usb 3/3, desktop-shell, f5-core 2/2, vm-window, f1-input 24/24, audio nativo 2/2, dos4gw, dpmi, vm-boot, vm-video, safe-remove-usb, optical-l1; rossi noti su TCG: quake (`exit error=-110`, f3-30), vm-audio, vm-fullscreen (f3-29), rtl8139 `firmware_active`; aperti di sola topologia di prova: mount-crash-reboot (anche con la pausa di f1-75 una scrittura non tracciata precede i record `case=cut`; sul T23 la riparazione dopo taglio è coperta dalla voce "Mount repair" della pagina H, PASS) e volume-resolution-cd-usb (due unità ATAPI sul secondario di QEMU; il T23 ne ha una, ora riconosciuta da f1-77).
- Sul T23: pagina H (voce "SMI bit 10 s" con periodo misurato e registri, `kind=atapi` nel log); avvio con chiavetta inserita; uscita dallo schermo intero; spegnimento forzato e riavvio con il log dell'avvio precedente completo.

## Riferimenti

- `legacy/local/physical/a79f840c/` (seriale e log su disco, non pubblicati); `build/codex/f1-77…f0-35-report.md` (locali).
