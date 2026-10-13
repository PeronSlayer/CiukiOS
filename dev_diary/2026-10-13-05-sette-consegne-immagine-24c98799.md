# Sette consegne Codex fuse; immagine 24c98799 per il T23

- Data: 2026-10-13
- Tipo: modifiche (kernel, desktop, test), immagine
- Base: main `f50c831b`; immagine QEMU `build/f0/ciukios.img` `ebe0ac69…`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `24c9879901bd050fbfb08aad5bd1d0ad4cc4d8e09c490e4a3d46818f091ad781`

## Cosa è entrato in main

- **f4-09** (20011bf9): AC'97 sul CS4297A, ordine di reset, ripiego a 48 kHz, prova DMA silenziosa, record `[audio] failure stage=…`.
- **f1-38**: Savage 2D abilitato tramite le porte VGA (3c3/3c2, CR38/CR39 e CR40.0 su 3d4/3d5), test del lock sulle porte, sonda dell'alias MMIO (`mmio_alias_dead`), `value=` in ogni record di passo, proprietà delle porte VGA passata fra biosvm e acceleratore.
- **f1-39**: letture FAT di catene contigue fino a 64 KiB, `cache_read_run`, SET MULTIPLE MODE + READ/WRITE MULTIPLE (EXT) con `rep insw`/`outsw`, letture native e snapshot eseguibili fino a 64 KiB, task di storage senza `storage_work`.
- **f1-40**: presentazione per righe (`rep movsl`, `rep stosl`), nessuna copia software sul LFB, `lfb_mb_s` nel record di presentazione intera; 69.648 confronti di riferimento.
- **f2-44**: desktop con colore pieno subito, sfondo decodificato e scalato da un thread, cache dei fotogrammi resi in `/System/cache/wallpaper/`, cinque fotogrammi pre-resi nell'immagine, record `[desktop] stage=…` in avvio verboso. Su host: decodifica 26 ms, scala 11 ms.
- **f2-45**: compositor per righe, cache delle barre del titolo, ombre per intervalli, blit di icone e glifi per righe; identico al pixel; su host 2,8–3,4x più veloce (obiettivo 4x non raggiunto: secondo giro dopo le misure sul T23).
- **f6-02**: nodo `/dev/net` di sola lettura con `NET_GET_STATE`, header SDK, icona di rete nella barra (Tango `network-wired`/`network-offline`/`network-error`), tooltip e popup, aggiornamento ogni 2 s, barra da 132 px.
- **f3-11** (parziale): consegna IRQ DPMI transazionale (ISR e contatori solo con frame costruito), record `[dosvm]` quando un bit in servizio resta attivo oltre 1 s senza EOI, stepping TF del monitor documentato; lo stallo del T23 non è stato riprodotto su host.

## Verifiche

- Suite desktop 80/80, gate 10/10, sviluppo nativo 12/12, dispositivi 2/2, modalità video 5/5, avvio normale 14/14, test host del kernel, payload 7/7, `make build-full` con albero pulito, f0-smoke 4/4.
- Correzioni ai test dopo la fusione (f50c831b): il replay harness reindirizza `open()` (la directory `/dev/net` dell'host si apriva e il descrittore collideva con l'oggetto di input del replay), fusione del test del launcher riparata, fixture con `folder.c`, conteggio nodi del gate con `/dev/net`, golden del compositor ri-registrati per la barra più larga.

## Da verificare sul T23 (cattura seriale)

`[accel] savage … value=` fino a `ready` o al motivo di quarantena; `[display] present full … lfb_mb_s`; `[launch-stage]` con `disk_max` ≥ 32768 e ELF da 1,3 MB sotto 0,4 s; `[desktop] stage=`; `[audio] … reason=` ed eventuale `failure stage=`; DOOM: record `[dosvm]` di IRQ in servizio; icona di rete con cavo inserito e tolto.
