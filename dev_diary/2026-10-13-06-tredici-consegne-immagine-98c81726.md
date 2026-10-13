# Tredici consegne fuse (prerequisiti F5 e parco driver); immagine 98c81726 per il T23

- Data: 2026-10-13
- Tipo: modifiche (kernel, driver, desktop, SDK, test), immagine
- Base: main `b303a5a9` (più la correzione del test dello stack); immagine QEMU `21106e99…`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `98c81726075ed4af6258fcfb4c9e856e976b4fd02303687d25c713ae092a014e`

## Cosa è entrato in main

- **f4-10** (Astra): AC'97 sul CS4297A: l'attesa delle sezioni analogiche pronte scade come avviso (comportamento di Linux `ac97_codec`), prova di risposta del codec, prova DMA silenziosa come cancello di `ready`, record `[audio] stage=` con i registri.
- **f1-41**: Savage: test del lock funzionale su CR40.0 (il T23 rilegge 68h/A8h, bit chiave conformi a RE046-B), alias MMIO confrontato con la porta, CR66 sui bit definiti.
- **f3-12** (Astra): stallo di DOOM spiegato: i record diagnostici sincroni sulla seriale emessi dentro il gestore IRQ del guest (52 ms per riga a 38400 baud) provocavano IRQ0 ricorsivi fino al quarto gestore annidato di DMX, che usciva senza EOI; i record vengono bufferizzati fino al ritorno del gestore più esterno. `00ff:0020` è lo stub di DOS/4GW.
- **f2-46** Terminal, **f2-47** Registry (libreg, `/System/Registry/*.reg`, radice Hardware, Registry Editor), **f2-48** compositor secondo giro (parziale: 1,14–1,23x su host), **f2-49** font TrueType (libfont su stb_truetype 1.26, Liberation 2.1.5 verificata byte a byte contro l'archivio upstream, pagina Fonts).
- **f6-03**: censimento dei binari del pacchetto driver e contratto dell'host NDIS; conclusione: i miniport NT5 utilizzabili aggiungerebbero solo RTL8169, PRO/1000, BCM440x e PCnet per 126 import; si preferiscono i driver nativi, host in archivio.
- **f1-42** USB (UHCI/OHCI, hub, HID tastiera e mouse, instradamento EHCI ai companion), **f1-43** memorie di massa USB (BOT/SCSI, volumi rimovibili da E:, rimozione sicura via `/dev/media`), **f1-44** ATA bus-master DMA (modo UDMA del firmware, PRD, bounce, ripiego PIO), **f6-08** cinque driver di rete (e1000, pcnet, tulip, 3c90x, rhine), **f4-11** sette famiglie audio (ES1370/1371, SB16, VIA, SiS, nForce), **runner-02** USB nel runner QEMU e suite `f1-usb`.

## Verifiche

Suite desktop 84/84, gate 10/10, sviluppo nativo, dispositivi, modalità video, avvio normale, payload 8/8, test host del kernel e del filesystem, test statico del loader, `make build-full` con albero pulito, f0-smoke 4/4. La suite `f1-usb` è stata rifiutata dal runner per un test host (stub `storage_get` duplicato nel test dello stack dopo f1-43, corretto in questa voce) e viene rieseguita sull'immagine.

## Correzioni del lead durante le fusioni

Conteggi di inventario e catalogo (Terminal, Registry Editor, dodici font con avvisi), tabella driver a sedici righe con `usb-host` e le righe NIC sostituibili, `pci[0]` ripristinato come NIC nel test dei dispositivi, header `media.h` nelle fixture del gate, golden del compositor ri-registrati per la barra con l'icona dei supporti, riferimento API rigenerato due volte.
