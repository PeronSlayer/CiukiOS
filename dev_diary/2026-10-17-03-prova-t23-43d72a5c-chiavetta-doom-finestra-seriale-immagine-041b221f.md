# Prova del T23 sull'immagine 43d72a5c: lag con la chiavetta, DOOM che spara, frame in finestra, seriale sparita; immagine 041b221f

- **Data:** 2026-10-17
- **Tipo:** analisi (hardware reale), modifica (kernel: filesystem, VM DOS, audio, scheduler, loader, seriale; desktop e Files), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `25c1e0df`; immagine QEMU `d6e41c72a2f056aa92d4cb186a772c155aaae2764f65596ca7dcd7105d7dea95`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `041b221f1d42de1dfc2c1d958b167f5becd053195d15ea157405302494635842`
- **Stato:** immagine consegnata alle 09:14 su richiesta del proprietario prima dei gate QEMU (eccezione esplicita), gate conclusi alle 10:00 con i soli rossi noti; prima prova del proprietario fatta, tre difetti aperti (sotto)

## Contesto

Prova del proprietario sull'immagine `43d72a5c` (voce 2026-10-17-02), log su disco in `legacy/local/physical/43d72a5c/` (tre file; la sessione dei giochi è ruotata e resta solo nella scatola nera di BOOT2). Esito riferito: ingresso e uscita dallo schermo intero ripetuti senza errori; giochi in finestra con cali di frame; con la chiavetta Files mostra il contenuto ma il sistema rallenta e un click dentro la chiavetta porta Files in "Not responding"; in DOOM sparare o aprire una porta fa scattare il gioco e distorce e rallenta l'audio, in finestra e a schermo intero. Cattura seriale vuota.

- **Chiavetta**: `[desktop] stall stage=shell … wait=media-read … folder_wait=directory-compare` fino a `no_frame_ms=3266` con `[cpu] top=files 22–44 %`: il thread dei fotogrammi attende in `media_read` (26 `MEDIA_GET_VOLUME`, ciascuno prende `vfs.lock`) dietro a Files che legge E: tenendo il blocco unico del filesystem; Files elenca sul thread dell'interfaccia con `stat` per voce e lettura dei primi byte di 128 file (`FM_LIST_SNIFF`); il ping del desktop non riceve risposta per 5 s.
- **DOOM**: `[vm-audio] sb state=running reason=irq_late cmd=b6 dma=5 hz=11025` ×73 e `state=irq_wait reason=complete` ×118; `irq_wait ms=501 irq=7 isr=0000 irr=0083`: il modello SB16 fermava la DMA auto-init in attesa dell'acknowledge dell'IRQ, cosa che l'hardware non fa (la DMA continua, l'IRQ resta latched, un bordo in più non genera un nuovo fronte).
- **Finestra**: `vm_perf_share vm_permille=613–778 guest_permille=121–486`; `stretch_max_ms=28–31` in `dosvm_vga_present`, `cvga_render_row8`, `memcpy` (presentatore) e `present_xrgb_row` (compositore); `134 ms` nel display-watchdog in `fs_read` ai checkpoint durevoli di ogni cambio schermo (una tantum).
- **Seriale**: `boot_flags=00001a08` in tutti gli avvii (il loader non ha trovato la COM1); nell'immagine d598bf4c `1a0a` ma `UART absent` (porta trovata e poi disattivata); al mattino (a79f840c) funzionava. Nessuna modifica del loader fra le tre immagini: da verificare l'impostazione BIOS Config → Serial Port del T23.
- Il PC di sviluppo è andato in sospensione nella notte: il lavoro è ripartito alle 07:36 con un blocco della sospensione (`systemd-inhibit`).

## Decisioni

- Blocco dello spazio dei nomi separato da 26 blocchi FIFO per lettera; l'I/O del filesystem avviene fuori dal blocco dello spazio dei nomi; le operazioni su più percorsi riservano l'insieme di unità in ordine crescente (ordine: nomi → volume → cache → dispositivo). Le interrogazioni `MEDIA_GET_*` copiano un'istantanea pubblicata e non attendono mai un volume.
- La DMA auto-init del SB16 virtuale avanza sul timer fisico attraverso i confini di blocco indipendentemente dall'acknowledge; un confine con la linea ancora alta conta `irq_merged`; l'IRQ è iniettato al primo confine utile e di nuovo al ritorno finale con IF spento; latenza dell'ISR misurata; record `cadence` ogni 5 s.
- Files elenca e naviga su un lavoratore cancellabile con stato "Loading…"; elenco economico su ogni volume; riconoscimento ELF solo all'apertura; l'osservazione dei supporti del desktop passa per una cassetta su lavoratore.
- Il loader registra in `boot_info` byte 0x00F il passo della rilevazione UART (0–7) e azzera il flag seriale se spegne la porta; il kernel riprova la COM1 e registra `[serial] loader_uart …` / `kernel_probe …`; la pagina H nomina il passo.
- Il presentatore della finestra e il compositore raggiungono un punto di preemption ogni quattro righe o ogni tessera da 4 KiB leggendo i piani VGA dal vivo (strappo accettato in finestra, documentato); nessuna lettura di orologio sul percorso cooperativo di produzione; i modi 13h/X/Y si campionano a metà refresh solo quando il modello è cambiato; una VM a fuoco con IRQ pendente ha la precedenza al prossimo punto sicuro. La prima consegna con istantanea dei piani (12× il costo del presentatore) è stata respinta.

## Cosa cambia

- **f0-38** (`84901a3d`): passo UART, riprova del kernel, pagina H; loader ancora 24576 byte.
- **f3-40** (`45506177`): DMA libera, `irq_merged`, iniezione al confine, latenza, record di cadenza; replay host 2501 casi con cadenza esatta e zero underrun.
- **f1-79** (`5478653a`): blocchi per volume, chiamanti nativi migrati, istantanea dei supporti, `vfs_wait`; test host a tre thread: ogni chiamata del volume veloce e dei supporti ≤ 1 ms durante la scansione lenta.
- **f2-80** (`fa483571`): lavoratore di Files, elenco economico, sniff all'apertura, supporti del desktop su lavoratore; host: primo disegno 3 ms, ping 2 ms con 300 voci a 25 ms per chiamata.
- **f3-41** (`06d5ab49`): punti di preemption, campionamento a 35/30 Hz su cambiamento, precedenza IRQ; replay host di DOOM: presentatore 82 → 66 µs per aggiornamento, tratto massimo 30 → 4 ms, quota dell'ospite invariata entro 0,003 punti.
- Direttive: `docs/directives/f0-38`, `f3-40`, `f1-79`, `f2-80`, `f3-41`.

## Verifica

- Suite host per ogni consegna (f0-38: boot modes 13, loader model 14, serial 3, hardware check 7; f3-40: audio 11,1 milioni di controlli, VM 23,9 milioni; f1-79: filesystem, USB, power-cut 3,2 milioni, namespace 23.605; f2-80: desktop 104/104, gate 11/11; f3-41: suite host completa, replay cpustat 4 ms, 24/24 eventi desktop sotto pressione IRQ); build del kernel su main dopo ogni fusione.
- Catena Z9m sull'albero `25c1e0df`: sdk, desktop 104/104, gate 11/11, apps, build-full con albero pulito (immagine QEMU `d6e41c72`), suite host del kernel, payload 9/9, immagine T23 `041b221f`, f0-smoke 4/4.
- Gate QEMU Z12m sull'immagine `d6e41c72`: verdi fat32-read, cache, fat-write, mount-dirty, **mount-crash-reboot** (per la prima volta: il banco di prova del taglio ora cade nel punto dichiarato con i blocchi per volume), bootlog, bootlog-read-only, fd-table, libc-smoke, f1-usb 3/3, desktop-shell, f5-core 2/2, vm-window, f1-input 24/24, audio nativo 2/2, dos4gw, vm-boot, vm-video, dpmi, safe-remove-usb, optical-l1; rossi i cinque casi noti con le stesse ragioni (vm-fullscreen su VGA standard di QEMU, vm-audio deadline TCG, quake `exit error=-110`, rtl8139 `firmware_active`, volume-resolution-cd-usb con due unità ATAPI).
- Prova del proprietario sul T23 (riferita alle 09:50): la chiavetta è riconosciuta e le cartelle si navigano; aprendo un file musicale dalla chiavetta il lettore si apre ma non suona; dopo qualche minuto le porte USB smettono di funzionare, mouse compreso; DOOM invariato quando spara; chiudendo il gioco a schermo intero il sistema diventa inutilizzabile e serve lo spegnimento forzato. Log su disco in attesa di copia.
- Sul T23 (da fare): avvio `N` per i giochi (con `L` la diagnostica costa CPU); chiavetta inserita all'avvio e a caldo, navigazione dentro la chiavetta in Files; DOOM con spari e porte in finestra e a schermo intero (record `[vm-audio] cadence …` nel log); Wolf3D e Quake in finestra; pagina H con la riga `UART absent (loader step N, kernel probe M)` se la seriale manca ancora.

## Riferimenti

- `legacy/local/physical/43d72a5c/` (log su disco, non pubblicati); `build/codex/*-report.md` (locali).
