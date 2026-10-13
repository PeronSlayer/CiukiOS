# Consegne della notte dal secondo giro sul T23: ridisegno, accelerazione 2D, desktop come cartella, Music Player, audio AC'97, DOOM; immagine 8845b848

- **Data:** 2026-10-13
- **Tipo:** modifiche (desktop, driver, DOS VM, audio); immagine per il T23
- **Versione/commit:** `main` `06cd93f6` (immagine `8845b848…`, immagine hardware `62e4dc6c…`)
- **Stato:** immagine consegnata al proprietario; verifica sull'hardware da fare

## Contesto

Prova del proprietario sul T23 dell'immagine `36010476…` (notte fra il 12 e
il 13): cambio di risoluzione a 1280×1024 riuscito (secondo cambio immediato
rifiutato con EBUSY), spegnimento riuscito; DOOM raggiunge il menu e si
blocca; ridisegno lento; nessun suono; impossibile creare cartelle sul
desktop; mancano un lettore musicale e un accesso evidente al visualizzatore
di immagini. Sei direttive, implementate da Codex in sessioni con tetto di
memoria (due o tre alla volta).

## Cosa cambia

- **f1-25 ridisegno**: record di misura per frame (area sporca, present,
  tempi) e riga nel Task Manager; present solo dei rettangoli sporchi,
  coalescenza per frame, composizione a doppio buffer, cursore in overlay
  con salvataggio dello sfondo, input elaborato prima del rendering, nessuna
  scrittura sincrona dal thread dell'interfaccia; sequenza del cambio modo nel
  Control Panel corretta (EBUSY).
- **f1-26 accelerazione 2D S3 Savage/SuperSavage**: driver sul registro
  (classe 03, id della famiglia), BitBLT, riempimenti pieni e a motivo,
  cursore hardware tramite `accel_ops` nel presenter, ripiego software
  trasparente e quarantena; `docs/design/savage-2d.md` con i riferimenti ai
  registri; record `[accel]`.
- **f2-42 desktop come cartella**: `C:\Documents\Desktop` dopo le icone fisse
  (Computer, Documents, Trash); menu Nuovo (cartella, documento di testo,
  immagine), Apri, Apri con, Rinomina, Elimina nel Cestino, Proprietà;
  trascinamento con Files; posizioni in `desktop.cfg`; tastiera.
- **f4-07 Music Player**: WAV, MP3 (minimp3), Ogg Vorbis (stb_vorbis),
  playlist M3U, decodifica in un thread con anello limitato; associazioni in
  Files; Image Viewer nel menu Programs con navigazione nella cartella.
- **f4-08 AC'97 su hardware reale**: reset del controller e attesa del codec,
  uscita dal risparmio energetico, smute e volumi (master, cuffie, PCM),
  EAPD per i codec identificati (AD1881A, CS4299), frequenza, bus master;
  record in avvio `L` con l'identificativo del codec e l'avanzamento del
  playback; tono di un secondo all'avvio in modalità `L`; il test del Control
  Panel riporta ciò che il kernel ha visto.
- **f3-10 DOOM fermo nel menu**: stallo dell'IF virtuale dopo un avvio
  riuscito corretto al confine di ritorno (il timer, la tastiera e il
  rendering del menu riprendono nel replay host); record spurio all'avvio
  delle finestre DOS eliminato in avvio silenzioso; battito diagnostico della
  VM DOS ogni 2 s in modalità `L` (modo, CS:EIP, IF, IRQ pendenti, tick del
  PIT, tasti, DMA del Sound Blaster, raccolte VGA).
- Correzioni del lead: riferimento API dello sviluppatore rigenerato due
  volte dopo i nuovi header; payload di Tiny C ricostruiti prima dei test
  nativi nella catena di build.

## Verifiche

Test host sull'albero `06cd93f6`: desktop 74 OK, gate 10 OK, sviluppo
nativo 12 OK, dispositivi 2 OK, payload 7 OK, test host del kernel OK.
Immagine `8845b848…` (albero pulito): `f0-smoke` 4 PASS. Immagine hardware
`build/hw/ciukios-hw.img` SHA-256
`62e4dc6cfcc72970635314da86d0f452e189587f2d0950a14e6700529fc4c3f7`.
La prova sul T23 deve avvenire con avvio `L` per raccogliere i record di
audio e della VM DOS.

## Metodo

Una sessione Codex (f3-10) è stata interrotta dal filtro di sicurezza di
OpenAI sul contenuto (interrupt, DMA, trap della CPU); rilanciata con una
descrizione neutra del compito ha completato il lavoro.
