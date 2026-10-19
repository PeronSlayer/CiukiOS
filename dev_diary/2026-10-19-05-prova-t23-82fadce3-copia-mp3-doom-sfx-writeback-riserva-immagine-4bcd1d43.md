# Prova del T23 sull'immagine 82fadce3: copia che blocca il sistema, MP3 che balbetta, effetti di DOOM tagliati; writeback a budget, riserva del lettore, copia dal desktop su worker, frame SB in ritardo suonati; immagine 4bcd1d43

- **Data:** 2026-10-19
- **Tipo:** analisi (hardware reale), modifica (kernel: cache dei blocchi, sorgente audio VM; applicazioni: Music, desktop, Files, toolkit), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `c180f28e`; immagine QEMU `08874ef2b3f7857039e4bfbb79fa43bd2e7a1b1da2df511b64c59fea0d89726e` (non usata per i gate: voce 2026-10-19-03); immagine T23 `build/hw/ciukios-hw.img` SHA-256 `4bcd1d43b26969c5e2c06b16547bae9947086bf5e271b8c0551b0c3f7d7e505b`
- **Stato:** immagine consegnata; verifica sul T23 in attesa

## Contesto

Prova del proprietario sull'immagine `82fadce3` (voce 2026-10-19-02; consegnata alle 11:50 su richiesta, con i gate QEMU completati verdi alle 12:30 e la suite audio intera verde per la prima volta), log in `legacy/local/physical/82fadce3/` (seriale 5571 righe, un avvio). Esito riferito: DOOM funziona senza terminazioni, ma gli effetti sonori a volte si tagliano; incollare un file dalla chiavetta sul desktop rende il sistema instabile finché la copia non termina e non compare alcuna finestra di avanzamento ("sembra che più letture e scritture contemporanee non si possano fare"); la riproduzione di un MP3 dal disco durante la copia "va stra lentissimo e lagga".

Dal log seriale:

- **Trasporto audio sano** per tutta la sessione (`[audio] transport running published=24577 retired=24569 halts=0 underruns=0 lead_periods=8`): i difetti udibili non sono nel DMA.
- **Copia**: 28 record `[storage] vfs_wait drive=C ms=100–740 holder=C reason=volume`; `files#25` al 30–59 % della CPU e `desktop#17` al 17–51 %, `idle_pm=0–2`. Nessun record `[files] job`: la copia è passata dal percorso del desktop (`apps/desktop/folder.c:243–253`, 24 passi sincroni da 16 KiB per turno di servizio sul thread della shell, senza finestra), non dal worker di f2-84. Nel kernel la cache dei blocchi (8 MiB) si riempiva di dati sporchi: il writeback partiva solo dopo 5 s (`cache_writeback_tick`) e, con la cache piena, chi chiedeva un blocco nuovo scriveva la vittima sporca sul dispositivo in modo sincrono (`cache.c:244`) tenendo la prenotazione del volume; i lettori dello stesso volume aspettavano dietro quella prenotazione.
- **MP3**: `[music] open … ms=297`, `decode … first_pcm_ms=351`; il decoder leggeva 16 KiB alla volta e il lettore teneva al massimo 256 frame decodificati (5 ms, limite di f4-31): ogni attesa oltre i 43 ms della coda del kernel diventava un buco udibile.
- **DOOM**: cadenza sana (`irq_latency_avg_us=901–1142`, `late_acks=0 merged=0`). Il contatore `native_refill … late=70` indicato come possibile causa è, come ha verificato Codex, il conteggio degli IRQ SB fusi (`sb.late`), non dei frame della sorgente scartati; la causa degli effetti tagliati resta da stabilire sul T23 con i nuovi contatori.

## Decisioni

- **Cache dei blocchi (f1-82, contratto C1).** Writeback in background a budget per dispositivo (`min(cache/8, 1 MiB)`) o per età (1 s) in lotti ordinati per LBA di 64 blocchi al massimo, con trasporto e flush fuori dal mutex della cache; l'allocazione preferisce vittime pulite e, con sole vittime sporche, sveglia il writer e aspetta al massimo 20 ms prima del vecchio percorso sincrono (contato in `cache_sync_evictions`); chi scrive oltre 2 MiB di dati sporchi dorme in `cache_write` finché il dispositivo torna sotto budget e, senza progresso per 20 ms, assiste con scritture limitate; record protetti `[storage] writeback …` ogni 5 s con dati sporchi presenti; `storage_writeback` prenota un volume solo per pubblicare un errore. Ordinamento FAT, barriere di durabilità, errori per dispositivo e revoca in sola lettura invariati. Limite dichiarato: un chiamante limitato conserva la prenotazione FIFO del volume (f1-79); l'equità dei lettori sullo stesso volume sotto pressione severa è oggetto di f1-83/f1-84. Audit dell'allocazione: 48 sonde per append da 64 KiB con l'hint `next_free`, sotto 1 ms di CPU, nessuna riscrittura necessaria.
- **Lettore (f4-34).** Thread di lettura/decodifica separato con anello PCM da 512 KiB (131072 frame, 2,73 s a 48 kHz), buffer stdio di lettura anticipata da 256 KiB per codec, avvio dopo 100 ms decodificati o EOF, prefill del dispositivo invariato a 256 frame, posizione dal cursore udibile e non dai frame decodificati, stato "Buffering…" con record `[music] starve ms=… reservoir_frames=…` (al massimo uno al secondo) e conteggio in `[music] close`.
- **Trasferimenti del desktop (f2-85).** Worker condiviso estratto in `apps/files/transfer.c` e usato anche dalla cartella del desktop; finestra di avanzamento e prompt di sostituzione reali (Sì / Sì a tutti / No / Annulla) ospitati dalla shell; nuovo messaggio v2 `DESK_CLOSE_ACK`: un client che risponde "chiudo dopo" entro 2 s ottiene fino a 30 s; Files aggiorna l'elenco al massimo una volta al secondo e ridisegna solo le righe cambiate.
- **Sorgente audio delle VM (f4-35).** I frame Sound Blaster in ritardo vengono suonati in ordine con uno spostamento comune della linea temporale (PCM ed eventi OPL/DAC pendenti) entro un budget di 20 ms per sorgente; oltre, scarto registrato; istogramma dei ritardi (< 1, 1–5, 5–20, > 20 ms) e istantanee lead/lag allo scarto nel record di cadenza; nessuna modifica all'anticipo del trasporto. Analisi dei clock: PIT e DSP avanzano con gli stessi `g_ticks`; i record non separano tick fisici persi da errori di frequenza del codec, servono il confronto con un orologio esterno e l'ascolto sul T23.

## Cosa cambia

- **f1-82** (`59da52b9`, fusione `c180f28e`): `cache.c`, `cache.h`, `mount.c`, `vfs.c`, sonda FAT; replay host (copia da 10 MiB a 1 MiB/s con lettore da 16 KiB ogni 20 ms sullo stesso volume): attesa massima del lettore 300,8 → 16,9 ms, comandi di scrittura 21478 → 1278, copia alla velocità della sorgente; suite fs con fsck e mtools e 1.975.157 asserzioni di taglio dell'alimentazione verdi.
- **f4-34** (`0b82918b`, fusione `98d94543`): modello host con stalli di 500 ms al secondo: buchi 17/14/16 → 0 per WAV/MP3/Ogg, costo del decoder 30,7 → 30,0 µs per frame MP3.
- **f2-85** (`35e7d3f5`, fusione `00b52798`): replay host della cartella del desktop con I/O bloccato: 0,5 % di CPU del thread UI e 0,03 ms di turno massimo; Files con un altro processo che scrive nella cartella mostrata: 5,4 % → 2,9 %; catture `build/codex/f2-85-frames/`.
- **f4-35** (`8d73a1d6`, fusione `6a8ef9cb`): fixture SB16 di produzione con jitter: 90 → 0 frame scartati con ritardo aggiunto ≤ 10,6 ms; caso QEMU `vm-audio-jitter-qemu-t23` scritto e non eseguito; asserzione di pacing resa relativa (approvata dal lead).
- Direttive: `docs/directives/f1-82`, `f4-34`, `f2-85`, `f4-35`.

## Verifica

- Suite host per ogni consegna; sull'albero fuso: suite host del kernel verde dopo f4-35, build delle app e 112 test desktop verdi dopo f2-85, suite fs e audio verdi prima della build finale (catena Z9u-pre).
- Catena Z9u sull'albero `c180f28e` (ciclo senza QEMU): suite fs e audio verdi, sdk, desktop, test desktop 112/112, gate 11/11, apps, build-full con albero pulito (immagine QEMU `08874ef2`), suite host del kernel verde, payload 9/9, immagine T23 `4bcd1d43`.
- Sul T23 (da fare): avvio `L`; incollare un MP3 dalla chiavetta sul desktop (finestra di avanzamento, risposta del sistema durante la copia, Annulla); riprodurre un MP3 dal disco durante una copia (nessun "Buffering…" o pochi episodi); DOOM a schermo intero per alcuni minuti con attenzione agli effetti (istogramma nel record di cadenza); log con `mcopy -n -o -i /dev/sdc1 '::/system/logs/*.LOG'`.

## Riferimenti

- `legacy/local/physical/82fadce3/` (seriale e log su disco, non pubblicati); `build/codex/f1-82-report.md` e `f1-82-logs/`, `f4-34-report.md`, `f2-85-report.md` e `f2-85-frames/`, `f4-35-report.md` e `f4-35-logs/` (locali); `docs/design/win98-class-resource-management.md` (voce 2026-10-19-04).
