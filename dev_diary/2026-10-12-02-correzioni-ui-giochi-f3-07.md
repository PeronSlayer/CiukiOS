# Correzioni grafiche del proprietario, giochi DOS verdi su QEMU, f3-07 e revisione del kernel

- **Data:** 2026-10-12
- **Tipo:** modifica (UI, kernel VM), verifica QEMU, decisioni
- **Versione/commit:** `wt/integ2` da `755a4954` (fusione di `wt/ui-polish` `e9e8e446`) a `5ca58877`; `bb496d0c` fusione di `wt/f3-07` (`01548d2e`)
- **Stato:** immagine `639bcca5…` (look corretto) in qualifica; f3-07 fuso, QEMU da eseguire

## Contesto

Il proprietario ha visto le catture dell'immagine integrata del mattino
(voce 2026-10-12-01) e ha chiesto due correzioni: il pulsante Start non
deve stare nella barra in basso come in Windows ma nella barra in alto a
sinistra, più curato di un rettangolo; le title bar delle finestre devono
essere più belle e uniformi. In seguito ha chiesto di togliere la tagline
dalla barra in alto. Nel frattempo Codex è tornato disponibile al 100 %
(direttive f3-07, f2-34 e una revisione del kernel, voce precedente).

## Cosa cambia

1. **Pulsante Ciuki nella barra in alto** (`apps/desktop/shell.c`
   `draw_start`, `desktop.h` `DESK_START_*`): riquadro (4,4,96×24) con
   faccia arrotondata a gradiente, ritratto approvato ridotto a 18 px e
   scritta "CiukiOS"; si illumina sotto il puntatore e resta premuto a
   menu aperto; in Classic 98 è un pulsante 3D in rilievo. Il menu scende
   dal pulsante (y 32) e i sottomenu si aprono a destra; la finestra Run si
   apre sotto il menu. La barra in basso tiene solo i pulsanti delle
   finestre e il vassoio.
2. **Un solo pittore di cornice** `desk_paint_frame` (`theme.c`,
   `theme.h`) per programmi, finestra DOS, finestre interne e dialoghi di
   libdeskui (`dialog.c`): title bar di 24 px (i dialoghi erano a 20),
   pinstripe su gradiente, titolo centrato in grassetto, tre pulsanti
   quadrati di 15 px con glifi 7×7 e stati hover/premuto; varianti Ciuki
   Blue e Classic 98 (pulsanti 16×14 senza hover). I dialoghi modali hanno
   il solo pulsante di chiusura.
3. **Test coerenti**: euristica `normal-boot-desktop-2` in
   `scripts/test/run.py` (regione del pulsante nella barra in alto; le
   regioni della barra partono a destra del pulsante); modello geometrico
   in `tests/host/desktop/shell_menu.py` con `--regenerate` che riscrive i
   percorsi dei casi script (Ciuki (40,16), Programs (40,50), righe a
   x 240: CiukNote 46, Files 68, Task Manager 90, DOS Box 112, Demo 134,
   Lua 156, Control Panel 178, Accessories 200, Developer 230); 49 test
   host del desktop e 14 di `test_normal_boot` verdi.
4. **Tagline rimossa dalla barra in alto** (`5ca58877`): resta nella
   finestra About; la barra mostra il titolo della finestra attiva o
   l'avviso.
5. **f3-07** (Codex `gpt-6.1-sol` xhigh, `01548d2e`): finestra lineare
   diretta per il modo 13h concatenato, anche per i client DPMI: al primo
   fault idoneo sull'apertura le 16 pagine A0000h–AFFFFh della VM vengono
   mappate scrivibili in user mode, seminate dai quattro piani; le pagine
   sporche vengono deinterallacciate nel modello prima di letture del
   modello, scritture di porta che cambiano modo/accesso, BIOS e VBE. Il
   record `case=video` guadagna `linear_maps`/`linear_harvests`;
   `faults`/`flushes` passano a `case=video_detail`. Le pine dei casi
   DOOM di `f3-workloads` chiedono `linear_maps ge 1` al posto di `maps`.
   Replay host di DOOM: 1710 gametic, 1714 flip.

## Verifiche

- Immagine `8362fa73…` (burst VGA f3-06 + IF virtuale al POPF, voce
  2026-10-12-01): `doom-qemu-t23` PASS (5 giri, 1710 gametic),
  `doom-sound-qemu-t23` PASS (audio: 4355 blocchi DMA, 557 468 frame,
  6699 scritture OPL, 0 fault DMA), `wolf3d-qemu-t23` PASS (5 giri di gioco
  con 211 flip ciascuno, digest identico `39d8aa3b…`, circa 30 300 fault e
  149 000 store serviti a raffica per giro).
- Immagine `639bcca5…` (look corretto): sessione screenshot di 11 catture
  (desktop, menu, Files, CiukNote, Task Manager, Control Panel,
  Calcolatrice, DOS Box) consegnate al proprietario; immagine T23,
  `f0-smoke` e i casi desktop/app/finestra DOS in esecuzione (esito nella
  voce successiva).

## Decisioni

- **f2-34, superficie di ridimensionamento**: la prima consegna di Codex si
  è fermata perché il contratto F2 rende in sola lettura ogni superficie
  trasferita (`proc/surface.c`). Decisione del lead: alloca il client; nessuna
  modifica al kernel o all'ABI. Il client crea la superficie della nuova
  dimensione, la dipinge e la allega al riscontro del CONFIGURE; il desktop
  continua a comporre la vecchia fino all'arrivo, poi la scambia in modo
  atomico; al massimo una sostituzione pendente per finestra. Addendum in
  `docs/directives/f2-34-window-resize.md`.
- **Revisione del kernel** (Codex, sola lettura, diff `169dde64..af6369cd`):
  otto rilievi da trattare prima dell'immagine hardware, raccolti nella
  direttiva `docs/directives/f3-08-review-fixes.md` (IF virtuale DPMI con
  TF azzerato dall'istruzione seguente e firme all'indietro; elementi REP
  parziali e forme LOCK nel burst VGA; `ciuki_power` che ignora un sync
  fallito al secondo tentativo, parsing di `_S5` senza PkgLength, SCI_EN
  non verificato; errori di avanzamento audio in coda). Codex verifica ogni
  rilievo con un test host prima di correggerlo.
- Le voci di diario le scrive il lead: la voce proposta da Codex per f3-07
  non è stata inclusa; i fatti sono in questa voce.
- **Giochi shareware pre-installati** (agente Opus, `wt/wolf-bundle`
  `398e2aeb`, fusione in corso): DOOM 1.9 shareware (`doom19s.zip`,
  SHA-256 `cacf0142…`) e Wolfenstein 3D 1.4 shareware (`1wolf14.zip`,
  SHA-256 `cb2a2ef7…`) in `third_party/games` con i testi di licenza,
  disco `C:\GAMES\CIUKIGAM.IMG` costruito da `scripts/build_games_disk.py`
  e voci Programs > Games. Il `VENDOR.DOC` di Wolfenstein 3D ([3][C])
  richiede il permesso scritto di Apogee per la distribuzione in un
  pacchetto software: la decisione se tenerlo nell'immagine pubblicata
  spetta al proprietario; la pubblicazione resta sospesa.
