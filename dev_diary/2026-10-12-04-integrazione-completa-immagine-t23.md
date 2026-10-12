# Integrazione completa della pre-alpha 0.9.0: immagine 8435a16a e immagine T23 893a83e0

- **Data:** 2026-10-12
- **Tipo:** modifica (fusioni finali), verifica host, consegna hardware; decisione di processo
- **Versione/commit:** `wt/integ2` `96027a71` (immagine) e `70315129` (correzione successiva del Control Panel)
- **Stato:** immagine consegnata al proprietario per la prova sul T23; campagna QEMU rinviata per sua decisione

## Cosa cambia (fusioni dopo la voce 2026-10-12-03)

- `a5da9148` **f2-36**: Files parte dalla vista "Computer" con le unità `C:`,
  `D:`… (`/mnt/<lettera>`), percorsi mostrati come `C:\Programs`, `dev`,
  `mnt` e `tmp` nascosti in `C:\`, Places Computer/Programs/Documents/
  System/Games; l'icona "Computer" del desktop apre questa vista.
- `4d050606` **icone ed etichette** (agente Opus): una sola misura Tango per
  posto (menu 16 px, toolbar di Files 22, dialoghi 32, desktop 48), icone
  distinte per Settings, Run, Ciuki Demo, giochi, Clock, Hello, libc smoke,
  Lua; tabelle della shell e `icon_map.c` allineate (test `test_icons`);
  il sottomenu Developer elenca solo i programmi di sviluppo; etichette del
  desktop in una faccia dedicata con alone scuro sfumato e selezione
  traslucida arrotondata. Il cattura-schermo host disegna dallo store Tango
  e può rendere su un file di sfondo.
- `96027a71` **tasti negli scenari di test** (agente Opus): cause di
  `keys=0`: su e500 il percorso di ingresso via firmware riordinava il
  tasto prima del clic di chiusura; su `files-qemu-t23` lo script cliccava
  prima che Files fosse pronto. Negli scenari il desktop trattiene l'input
  finché il programma lanciato mostra il primo frame e lo rigioca con la
  stessa spaziatura; Files, CiukNote e Task Manager si spostano alla
  posizione di test prima del primo frame; il runner distingue gli errori
  di campo dai contratti falliti e nomina il contratto.
- `654b807a`, `70315129` **Control Panel**: l'anteprima di uno sfondo
  immagine è resa alla dimensione dell'anteprima con l'immagine decodificata
  in cache e, alla prima apertura, decodificata su timer dopo il primo
  disegno: la finestra si apriva nera per secondi sotto TCG.
- `ad545e56` **shell**: lo sfondo immagine viene decodificato due giri del
  ciclo dopo il primo frame del desktop (sotto TCG deterministico il caso di
  avvio normale vedeva ancora lo splash 8 s dopo lo spawn).
- `8b434e11` runner: scadenza QMP come `TimeoutError`.
- Revisione Codex del kernel salvata in
  `docs/validation/2026-10-12-kernel-review-codex-169dde64-af6369cd.md`.

## Verifiche

Test host sull'albero `96027a71` a macchina quieta: desktop 56 OK, avvio
normale 14 OK, gate 10 OK, test host del kernel OK, runner 85 OK (2 saltati);
l'inventario Lua fallisce solo se eseguito prima della build.
Immagine `8435a16a…` costruita da `96027a71`; immagine hardware
`build/hw/ciukios-hw.img` SHA-256
`893a83e0f24b515b1daa24d06da8fb22fc2c434a805d7b48343624d06bdc3421`
(`BOOT.CFG safe=0 serial=1`) consegnata al proprietario. Sessione screenshot
su QEMU (13 catture, di cui 10 pubblicate in `docs/screenshots/0.9.0`): desktop con Ciuk2, menu,
Files in vista Computer, CiukNote, Task Manager, Control Panel,
Calcolatrice, menu Games, Wolfenstein 3D e DOOM in finestra.

Prove QEMU precedenti valide per le immagini indicate: `2fdd515a…` f0-smoke
4 PASS, giochi 3 PASS, `desktop-shell-qemu-t23` PASS; `5dd760ee…` DOOM PASS
con f3-08. Il caso `normal-boot-qemu-t23` su `5dd760ee…` è fallito con il
loader fermo 258 s a macchina in swap (7 GiB) per i test host di tre agenti
in parallelo alle suite: non è un difetto dell'immagine.

## Decisioni

- Il proprietario prova direttamente l'immagine `893a83e0…` sul T23; la
  campagna QEMU completa (f0-smoke, f2-desktop, f2-process, f3-vm,
  f3-workloads, f4) si esegue dopo, prima della fusione in `main`.
- Durante una campagna QEMU nessun agente esegue build o suite host: la
  macchina (14 GiB) va in swap e falsa le scadenze TCG.
- Dodici worktree già fusi sono stati rimossi con i loro rami; restano
  `wt/integ2` e, fino alla fusione in `main`, nessun altro.
