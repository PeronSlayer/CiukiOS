# Prova del T23 sull'immagine b51f034f: prenotazioni del volume di 50–1500 ms, refill audio in coda, VM in finestra accoppiata al desktop

- **Data:** 21 ottobre 2026
- **Tipo:** analisi (hardware reale), decisione (direttiva f1-87)
- **Immagine:** `b51f034f04aee486…` (main b7f2d383 più f1-86, f3-52, f2-86, f0-49c, f5-07D, f5-21b, f5-10C4, f5-07C); kernel `041cc3a3…`

## Esito osservato dal proprietario

La musica viene riprodotta ma con picchi di ritardo, e ritarda quando si muove una finestra; l'apertura di qualunque finestra provoca ritardo e instabilità; i giochi DOS ritardano in finestra ma non a schermo intero; l'audio di DOOM resta leggermente distorto.

## Misure (log su disco `BOOT.LOG`/`BOOT1.LOG` e cattura seriale a 38400, circa 400 s)

1. Trasferimenti ATA in DMA confermati (`dma=6519 pio=0`), servizio massimo 111 ms per comando, età massima in coda 863 ms.
2. 513 record `vfs_hold drive=C task=desktop op=other` (prenotazione del volume senza etichetta dell'operazione): 406 tra 50 e 200 ms, 91 tra 200 e 500 ms, 16 oltre 500 ms (massimo 1502 ms), fino a 4 processi in attesa. `storage-writer op=log_write`: 35 prenotazioni di 1633 ms in media (massimo 1837 ms) con 3 in attesa; `op=commit` 602 ms; `dosvm op=mkdir` 441 ms.
3. Il compito `audio-refill` supera il proprio budget in 192 finestre su 201; il trasporto audio non va mai in underrun (`lead_periods=8`), con allungamenti fino a 9 ms attribuiti al desktop: il ritardo udibile è il refill in coda dietro le prenotazioni del volume.
4. CPU non satura: inattiva circa 57 %, dispositivi 14,5 %, interattivo 11–16 %, normale 13–14 %, circa 22 800 cambi di contesto per finestra di 2 s. Il desktop resta nella classe normale (base 7) in 1814 campioni su 2015.
5. VM DOS in finestra: `descheduled` 416–4907 per finestra contro 53–431 blocchi audio, latenza massima dell'IRQ SB16 da 33 a 476 ms, underrun fino a 1229, `parked` 472–668 in alcune finestre; le attese sul volume della VM sono tutte a 0 ms. La VM non attende il disco: è deschedulata o parcheggiata quando l'IRQ è dovuto (accoppiamento al presenter in finestra; a schermo intero il percorso è diretto e il proprietario non vede ritardi). Un tratto non prelazionabile di 129 ms in `dosvm_pool.c:239` (`resolve`).
6. Uscite della VM sulle porte del PIT: 0040 593 816 uscite (10,4 s), 0043 296 908 (7,4 s), OPL 0388/0389 230 000 (3,7 s): circa il 5 % della CPU della sessione.
7. Sezioni a interrupt spenti di 2,1 ms non attribuite: 1554 nella sessione (circa 3,9 al secondo), con `firmware_max_us=2117`; lo scanner USB riporta 3,98 scansioni al secondo per controller. Savage ancora in quarantena (`bci_test_readback_failed`), composizione via software. Il file di scambio riapre le estensioni esistenti (f0-49c funziona: `existing=1`).

## Decisioni

- Direttiva `f1-87` (Codex Sol, xhigh): etichetta e sito per ogni prenotazione del volume; prenotazioni limitate (metadati soltanto, rilascio durante il trasferimento, scrittura del log a fette di budget) con accettazione a 20 ms sul modello host con tempi di servizio del T23; refill audio sul percorso dei lettori prioritari di C3 e buffer decodificato di almeno 2 s; VM in finestra disaccoppiata dal presenter (attesa limitata a un fotogramma, cadenza audio indipendente); tratto di `resolve` limitato a 5 ms; percorso rapido per le porte 0040/0043/0388/0389; esperimento di attribuzione delle sezioni di 2,1 ms con opzione `usbscan=0` da provare sul T23.
- La cattura seriale richiede `stty 38400` sulla porta dopo ogni riavvio della workstation (lo script del lead lo imposta da solo); il tasto L del menu di avvio spegne il log seriale e non va premuto.
