# Prova del T23 sull'immagine 041b221f: linea IRQ 11 in quarantena, chiusura di DOOM a schermo intero, latenza ISR del guest, seriale disabilitata nel BIOS; immagine eca4d773

- **Data:** 2026-10-18
- **Tipo:** analisi (hardware reale), modifica (kernel: registro IRQ, rete, USB, VM DOS, display, loader diagnostica), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `99fefbcd`; immagine QEMU `5484f2a009c4e2c88beb301d1921483d518e8212796ecb6b79f7cea295ecf8a8`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `eca4d773658a8cb344f2eddc08c86a5254381d149eab636bc09147a98e1692c4`
- **Stato:** immagine consegnata; verifica sul T23 in attesa

## Contesto

Prova del proprietario sull'immagine `041b221f` (voce 2026-10-17-03), log su disco copiati il 18 ottobre in `legacy/local/physical/041b221f/` (BOOT1 = pagina H, BOOT = avvio Normale con chiavetta, lettore musicale e DOOM; la prova precedente aveva ruotato BOOT2). Esito riferito: chiavetta riconosciuta e cartelle navigabili; aprendo un MP3 dalla chiavetta il lettore si apre senza suonare; dopo qualche minuto porte USB e mouse USB smettono di funzionare; DOOM invariato quando spara; chiudendo DOOM a schermo intero il sistema diventa inutilizzabile (spegnimento forzato).

- **Linea IRQ 11** (BOOT 391–401): `[net] driver=e100 … mode=irq state=quarantined reason=irq_or_registry_fault`, `[irq] budget exceeded irq=11 owner=net-1690 us=2123`, `[irq] quarantined irq=11 owner=ac97/net-1690/usb-1694/usb-1698/usb-1702`, `[usb] quarantine controller=… reason=lease_invalid` ×3, `[input] … usb=0`, `[storage] readonly reason=device_quarantined file=….mp3`, `[audio] idle-tail transport stop err=-5`. L'handler dell'e100 (`net.c:135`) considerava colpa propria uno sforamento di 50 µs; lo stallo SMI periodico del T23 (2,1 ms, f0-35) è caduto nel suo handler; `registry.c:457` metteva in quarantena l'intera linea condivisa per un membro non più attivo. Lettore muto e USB morte sono la stessa cascata.
- **Chiusura di DOOM a schermo intero** (coda di BOOT): `fullscreen_watchdog reason=return-or-fault` a tick 116344, `guest-revoke` 118495, `vga-capture` 120653, `desktop-restore` 120860, `native-restore-streams` 121023, poi nulla; `fullscreen_log error=-11` a due passi; desktop fermo 7,9 s; `stretch_max_ms=151–160 task=dosvm eip=fs_read+0xd` per sette finestre e `133 task=display-watchdog eip=native_read+0x10`. `fs_read`/`native_read` sono le callback dell'apertura video a schermo intero (`dosvm_fullscreen.c:32`, `supersavage_display.c:30`), non il filesystem: le quattro copie da 64 KiB dei piani VGA (`vga_hardware.c:79/101`) giravano senza punti sicuri su un'apertura lenta (524 ms nel modello), ogni checkpoint durevole chiamava `bootlog_sync` con timeout di 2 s (`bootlog.c:443`, EAGAIN) e un handshake di parcheggio fallito abbandonava il ritorno (`dosvm_fullscreen.c:243`). L'ipotesi iniziale (rotazione a vuoto del blocco FIFO) non si riproduce: le attese bloccano correttamente.
- **DOOM che spara** (record `[vm-audio] cadence` di f3-40): `blocks=431 late_acks=49 merged=193 ack_max_ms=60 irq_latency_max_us=59602 irq_latency_avg_us=11354 descheduled=592 parked=0 if_clear=2014`: con la DMA libera non ci sono più underrun lato host, ma l'ISR del guest arriva in media 11,4 ms dopo il fronte perché per 2 s su 5 il monitor vede la IF virtuale spenta e per 0,6 s la VM non è schedulata.
- **Seriale**: `[serial] loader_uart state=2 base=0000` (nessuna COM1 nella BDA) e `kernel_probe result=3` (registro scratch a 3F8h non risponde); pagina H `SKIP Serial burst: UART absent (loader step 2, kernel probe 3)`. La porta è disabilitata nel BIOS del T23 (Config → Serial Port): non è software.
- Pagina H: `PASS USB storage: … read_ms=3073` per 1 MiB (340 KB/s), `FAIL USB mouse: events=0` (nessun movimento nei 5 s), timer `lost=78` con il noto stallo da 2,1 ms.

## Decisioni

- Un membro di una linea IRQ condivisa revocato, rilasciato o stantio viene staccato dalla catena (`[irq] member_detached …`) e gli altri membri continuano; la linea intera va in quarantena solo dopo 1000 passaggi non reclamati o per incoerenza del suo stesso record. Uno sforamento di 2,0–2,3 ms fra due campioni TSC con IF spenta e senza lavoro del driver è attribuito allo SMI e non conta; servono tre sforamenti non attribuiti in 100 ms per mettere in colpa un membro (stessa regola per `netif_irq`). Un controller UHCI/OHCI che perde l'IRQ passa al polling e conserva HID e memoria di massa.
- Le copie dei piani VGA e il trasporto DAC del provider a schermo intero servono lo scheduler ogni 256 byte / 16 voci; i checkpoint durevoli del ritorno sono accodati al log senza attesa; il parcheggio della VM è limitato a 250 ms e, scaduto, la VM resta sospesa al suo prossimo checkpoint mentre il desktop viene ripristinato.
- Il monitor misura per ogni vettore la durata dell'ISR vista dal monitor (consegna → ritorno osservato), campiona a 1 kHz il tempo con IF virtuale spenta e conta le sorgenti di riabilitazione (trampolino host, STI, IRET V86, POPF, API DPMI, recupero forzato); il recupero di un IRQ pendente non forza mai la IF dentro un'ISR hardware in corso. Il replay host di DOOM con suono attivo è già a 0,56 ms di latenza media e zero fusioni: il 40 % di IF spenta del T23 non si riproduce sul modello, quindi la causa del lag si decide con i nuovi contatori sulla prossima prova fisica.
- La diagnosi UART del loader (f0-38) ha dato il verdetto in una riga: impostazione BIOS, non codice.

## Cosa cambia

- **f0-39** (`e57867be`): `registry.c` stacco del membro, `irq_budget.c` attribuzione SMI e finestra a tre colpi, `net.c` stessa politica, `usb_core.c` polling su perdita dell'IRQ; contratti aggiornati; host: catena/registro, rete 1,7 milioni di controlli, USB.
- **f1-80** (`ed4e313d`): punti sicuri nelle copie VGA, checkpoint accodati, parcheggio limitato con ritorno forzato; modello: ritorno completo in 410–784 ms anche con VM che non parcheggia.
- **f3-42** (`b8fd7e46`): istogrammi di durata ISR per IRQ, campionamento della IF virtuale, contatori delle sorgenti di riabilitazione, guardia sul recupero, replay `isr:` di DOOM che spara e apre porte; suite VM 23,9 milioni di controlli.
- Direttive: `docs/directives/f0-39`, `f1-80`, `f3-42`.

## Verifica

- Suite host per ogni consegna; build del kernel su main dopo ogni fusione.
- Pre-verifica di main `5985c37b` (prima di f3-42): sdk, desktop, apps, suite host del kernel, desktop 104/104, gate 11/11, tutti verdi.
- Catena Z9n sull'albero `99fefbcd`: sdk, desktop 104/104, gate 11/11, apps, build-full con albero pulito (immagine QEMU `5484f2a0`), suite host del kernel, payload 9/9, immagine T23 `eca4d773`, f0-smoke 4/4.
- Gate QEMU Z12n sull'immagine `5484f2a0`: verdi fat32-read, cache, fat-write, mount-dirty, mount-crash-reboot, bootlog, bootlog-read-only, fd-table, libc-smoke, f1-usb 3/3, desktop-shell, f5-core 2/2, vm-window, f1-input 24/24, audio nativo 2/2, dos4gw, vm-boot, vm-video, dpmi, safe-remove-usb, optical-l1; rossi i cinque casi noti con le stesse ragioni delle immagini precedenti (vm-fullscreen su VGA standard di QEMU, vm-audio deadline TCG, quake `exit error=-110`, rtl8139 `firmware_active`, volume-resolution-cd-usb con due unità ATAPI).
- Sul T23 (da fare): BIOS → Config → Serial Port → Enabled, poi avvio `L` con cattura seriale; chiavetta con MP3 (il lettore deve suonare); mouse e USB dopo dieci minuti; DOOM con spari (record `cadence` nel log) e chiusura a schermo intero; pagina H.

## Riferimenti

- `legacy/local/physical/041b221f/` (log su disco, non pubblicati); `build/codex/f0-39-report.md`, `f1-80-report.md`, `f3-42-report.md` (locali).
