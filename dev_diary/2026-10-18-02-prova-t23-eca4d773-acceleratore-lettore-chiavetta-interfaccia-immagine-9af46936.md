# Prova del T23 sull'immagine eca4d773: schermo intero rifiutato dall'acceleratore, lettore MP3, velocità della chiavetta, nuova interfaccia; immagine 9af46936

- **Data:** 2026-10-18
- **Tipo:** analisi (hardware reale), modifica (kernel: USB, display, acceleratore Savage, registro; applicazioni: Music, libdeskui, desktop), decisione (interfaccia), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `287551c6`; immagine QEMU `2fc3c429df8a87e2ec94bdfc201ec1b42e2880228b932e563235f58904cd75f2`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `9af46936ed3df9e1776fcccae89d1578f9f0a61eca5e9b4b6a00127d9775e6ef`
- **Stato:** immagine consegnata; verifica sul T23 in attesa

## Contesto

Prova del proprietario sull'immagine `eca4d773` (voce 2026-10-18-01), log su disco in `legacy/local/physical/eca4d773/` (BOOT1 = menu L con chiavetta, lettore e giochi; BOOT = pagina H). Esito riferito: sistema stabile, porte USB e mouse vivi, chiavetta navigabile; lettura della chiavetta da velocizzare; il lettore musicale non carica l'MP3; la combinazione per lo schermo intero non fa nulla; richiesta una nuova interfaccia.

- **Schermo intero** (BOOT1 579–730): `fullscreen_request` arriva, poi `[registry] conflict: supersavage-mode wants 0:3b0-3e0 held by savage-vga reason=overlap` e `native_supersavage phase=claim result=-22`; l'acceleratore Savage, al primo avvio dopo la scrittura dell'immagine (marcatore "già tentato" assente), esegue il test BCI, fallisce (`bci_test_readback_failed`) e con `registry_quarantine` resta proprietario delle porte VGA. Dopo il ripristino `fullscreen_refused reason=busy … awaiting_frame=1` per 10 s (`desktop_frame_timeout`): le richieste successive sono rifiutate. f3-39 trasforma il rifiuto in continuazione in finestra, da cui "non accade nulla".
- **Lettore**: `[launch] name=music …` senza altri record: nessuna diagnostica. Sul modello host con volume a 25 ms per lettura l'apertura decodificava l'intero MP3 per contarne i campioni: 7,4 s e 291 letture.
- **Chiavetta**: `bind … reads=9 sectors=9 max_sectors=1` con `mount_ms≈250–330` (28 ms per comando), pagina H `read_ms=3073` per 1 MiB (340 KB/s); ogni fase BOT attendeva almeno due frame in `usb_retire_frames`.
- **Audio DOOM** dopo f3-42: `cadence … irq_latency_avg_us=808–1322 if_clear=2–118` per 5 s (prima 11.354 µs e 2014 ms).
- **Seriale**: ancora `loader_uart state=2`: porta disabilitata nel BIOS del T23.

## Decisioni

- Un acceleratore che fallisce rilascia le porte VGA dopo il proprio ripristino (CR45/CR40) o cede il lease fermo a `supersavage-mode` (presa in carico con prova, intervallo esatto, nuova generazione); il test BCI parte solo per le configurazioni qualificate con autorizzazione durevole a uso singolo; un rifiuto prima della revoca del framebuffer non lascia attese di fotogramma; il ripristino è riconosciuto dal primo present con la generazione corrente.
- I trasferimenti bulk USB si concludono al completamento (IOC sull'ultimo TD, verifica della scrittura del QH; OHCI: dequeue dell'ED e consumo del link di completamento), senza il minimo di due frame; le regole di recupero di f1-74 e il limite di 128 settori restano.
- Il lettore apre un MP3 dal primo frame (durata da Xing/Info o differita), carica su lavoratore con "Loading…", mostra l'errore di apertura, registra `[music] open/decode/error`; ogni `du_file_dialog` ha il pannello Places con i volumi montati.
- Interfaccia (decisione del proprietario, 2026-10-18): barra superiore con l'orb Ciuki (ritratto approvato, conversione deterministica, senza scritta), vassoio, notifiche e data/ora; barra inferiore con le sole finestre aperte; nessun titolo di finestra nella barra; tema Classic 98 rimosso (migrazione a Ciuki Blue), restano Ciuki Blue e Platinum. Seconda richiesta dopo le catture: orb più grande e sporgente alla maniera di Windows Vista, con l'incavo nella barra (f2-82). Il sorvegliante dello schermo del runner (`observe_desktop_screen`) aveva la geometria del vecchio pulsante cablata e falliva sull'albero fuso: ora legge la geometria dell'orb dallo stesso generatore del renderer.

## Cosa cambia

- **f1-81** (`771cbff6`): modello host READ(10) 30 → 3 ms, montaggio 300 → 57 ms, 64 KiB 457 → 941 KiB/s, elenco di 300 voci 44 → 20 ms.
- **f3-43** (`930c1374`): rilascio/presa in carico del lease, autorizzazione durevole del test BCI, handshake del ripristino.
- **f4-30** (`bd6ff22b`): avvio MP3 75 ms e 3 letture sul volume lento (WAV 151 ms, Ogg 177 ms), Places, diagnostica.
- **f2-81** (`b7d30a8f`): due barre, orb, rimozione di Classic 98; golden ridisegnati; catture `build/codex/f2-81-frames/`.
- **f2-82** (`c38e2cf2`, poi `03df2e54`): orb prima da 44 px (troppo grande per il proprietario) poi da 38 px che sporge 6 px sotto la barra in un incavo da 42 px, sfera lucida con il ritratto a 24 px, hit-test circolare sopra le finestre; `convert_portrait.py --geometry` genera `start-orb.h/.json` condivisi fra renderer e sorvegliante del runner; 40 catture in `build/codex/f2-82-frames/`.
- Direttive: `docs/directives/f1-81`, `f3-43`, `f4-30`, `f2-81`, `f2-82`.

## Verifica

- Suite host per ogni consegna; build del kernel, del desktop e delle app su main dopo ogni fusione; i golden delle schermate dei temi ri-registrati sull'albero fuso (f4-30 + f2-81).
- Catena Z9o sull'albero `df31084a` (f2-81 fuso): test desktop 106 con 1 fallimento e gate con 1 fallimento e 1 errore (`approved Ciuki portrait region mismatch`: il sorvegliante dello schermo del runner aveva cablata la geometria del vecchio pulsante Start); catena fermata, correzione in f2-82.
- Catena Z9q sull'albero `287551c6`: sdk, desktop 108/108, gate 11/11, apps, build-full con albero pulito (immagine QEMU `2fc3c429`), suite host del kernel, payload 9/9, immagine T23 `9af46936`; f0-smoke: i due casi `boot` verdi, `normal-boot-qemu-t23` fermo al menu del loader fino alla scadenza (schermo in modo testo con la riga di suggerimento, nessuna riga del kernel sulla seriale); ripetuto da solo dopo i gate: 4/4 verdi (`boot` ×2, `normal-boot-qemu-t23`, `normal-boot-qemu-desktop-2002`); episodio isolato non riprodotto, registrato qui.
- Gate QEMU Z12q sull'immagine `2fc3c429`: verdi fat32-read, cache, fat-write, mount-dirty, mount-crash-reboot, bootlog, bootlog-read-only, fd-table, libc-smoke, f1-usb 3/3, desktop-shell, f5-core 2/2, vm-window, f1-input 24/24, audio nativo 2/2, dos4gw, vm-boot, vm-video, dpmi, safe-remove-usb, optical-l1; rossi i cinque casi noti con le stesse ragioni.
- Sul T23 (da fare): BIOS → Serial Port → Enabled; avvio `L`; schermo intero al **primo** avvio dopo la scrittura (entrata, uscita, chiusura di DOOM a schermo intero); MP3 dalla chiavetta (il lettore deve suonare, Places nel picker); velocità di navigazione della chiavetta (`mount_ms` e `read_ms` nella pagina H); nuova interfaccia (orb, barre, orologio).

## Riferimenti

- `legacy/local/physical/eca4d773/` (log su disco, non pubblicati); `build/codex/f1-81-report.md`, `f3-43-report.md`, `f4-30-report.md`, `f2-81-report.md` e `build/codex/f2-81-frames/` (locali).
