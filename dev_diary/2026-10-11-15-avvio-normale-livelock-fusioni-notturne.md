# Avvio normale bloccato da un livelock del lock VFS; fusioni notturne verso la 0.9.0; primi screenshot del desktop

- **Data:** 2026-10-11 (sera e notte)
- **Tipo:** modifica (correzione del kernel e fusioni) e analisi di regressione; decisioni
- **Versione/commit:** `9de5efc` (lock VFS), `c139856` (worker), `50ba47f` (fusione di `wt/integ`); immagine `8e8d5dad…` (primo avvio normale al desktop)
- **Stato:** campagna 12 notturna in corso sull'immagine finale

## Contesto

Una sessione di screenshot dell'immagine `9fbfab4d…` avviata come la
avvierebbe un utente (nessuna richiesta di test) ha mostrato che il boot si
fermava dopo l'inizializzazione dello storage e il desktop non partiva. Le
campagne non lo rilevavano: ogni caso del runner avvia con una richiesta di
test via fw_cfg, e con quel flag `probes_main` non esegue `drivers_init`,
il disk log e il timer di writeback né `supervisor_bootstrap`.

## Cosa cambia

1. Diagnosi per strumentazione e campionamento di EIP via QMP: il task di
   avvio (priorità normale) prende il lock del VFS in `image_open` per
   leggere `/bin/desktop`; il timer dello storage (priorità dispositivo)
   accoda `storage_work`, che per scaricare il disk log chiede lo stesso
   lock; `fs_lock_take` attendeva con uno spin di `task_yield`, che cede
   solo a task di pari o maggiore priorità: il proprietario del lock non
   girava più. Il lock del VFS ora blocca su una coda `kwait` con sveglia in
   `fs_lock_drop` (regola f1-18), con attesa limitata a 1 s come il gate
   disco delle VM. Difetto presente dal disk log di F1, mai esposto.
2. Il rapporto "budget exceeded" del worker dispositivo alimentava il disk
   log, il cui scarico sullo stesso worker sforava di nuovo: ciclo che
   inondava console, seriale e disco. Il rapporto si limita ai primi otto e
   poi a uno ogni 1024 (il contatore resta nelle statistiche). Gli sforamenti
   di `storage_work` (3–5 ms) restano una voce aperta di F1.
3. Il runner ha un tipo di caso `normal-boot` (agente Opus): avvio senza
   fw_cfg, pietre miliari del loader, del kernel e del supervisore in
   ordine, fallimento su panic, record spuri, modalità sicura o desktop
   fermato, screendump dopo l'assestamento con verifica dei colori di barre e
   sfondo; due casi in `f0-smoke`, prerequisito di ogni suite.
4. Fusioni in `main` (`50ba47f`): f4-05 (audio delle VM verso il mixer
   AC'97 con il port C17 del DBOPL, decisione e licenze in
   `f4-acceptance.md`), f3-06 (sonde dos4gw/doom/doom-sound/wolf3d/
   vm-starve, disco fixture locale `CIUKIGAM.IMG` mai nell'immagine, fault
   VGA dal modo protetto al modello, TF del monitor V86 solo con shadow
   aperto o IRQ pendente), script dei casi app riscritti da una riproduzione
   host del desktop e delle app reali, correzioni della finestra DOS
   (INT 16h rieseguito con IF attivo quando un tasto attende nell'8042;
   console schermo spenta finché un desktop possiede il display; sonda
   `vm-window` con attesa di 120 s e verifica dello stadio campionato).
5. Prime catture reali del desktop sull'immagine `8e8d5dad…` via QMP:
   desktop con ritratto, menu Ciuki, Files, CiukNote, Task Manager,
   finestra DOS al prompt di CIUKIDOS.

## Verifiche

Immagine `8e8d5dad…` (avvio normale senza fw_cfg): `[supervisor] desktop
pid=2 pgid=2` raggiunto; nove screendump. Test host di `main` dopo le
fusioni: 344 OK nel ramo di integrazione prima dell'ultimo lotto; la suite
completa gira come prerequisito della campagna 12. Esiti della campagna 12
e dell'immagine hardware in una voce successiva.

## Decisioni

- Ogni campagna verifica l'avvio normale fino al desktop (`f0-smoke`).
- L'immagine hardware per il T23 avvia in modalità normale con log seriale
  (`BOOT.CFG` `safe=0 serial=1`), non in sweep.
- Il design visivo del desktop (f2-30/f2-31) è stato scelto dal lead senza
  revisione del proprietario: resta da sottoporgli con gli screenshot, con
  restyling contenuto nel toolkit se richiesto.
- Deviazione accettata per la 0.9.0: sintesi OPL3 nel worker kernel di
  riempimento invece di un worker ring-3 (contratto F4); livello mixer SB16
  predefinito 24 (Creative) da valutare all'ascolto.
