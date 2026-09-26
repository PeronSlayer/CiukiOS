# Chiusura della revisione UI di Claude — 25 settembre 2026

**Seguito su hardware reale:** il proprietario ha poi segnalato un blocco
del T23 con questa ISO. Safe graphics raggiunge il desktop; Live e SOUND
presentano problemi. Vedere la [diagnosi fisica e il nuovo candidato](t23-hardware-debug-2026-09-25.md).
I risultati qui sotto rimangono prove emulate, non una qualifica del T23.

Questa sessione riprende il lavoro descritto in
[ui-smoothness-2026-09-25.md](ui-smoothness-2026-09-25.md), partendo dai sorgenti
rimasti più recenti dell'immagine compilata. Le prove qui descritte sono in
QEMU o su istruzioni di produzione eseguite in Unicorn: non costituiscono una
qualifica del ThinkPad T23 o di una Radeon 9200 fisici.

## Correzioni successive alla consegna di Claude

- Il ritorno dal DOS conserva la finestra attiva e l'ordine delle finestre,
  senza riaprire Programs quando era minimizzata.
- Ridimensionando Programs sotto la larghezza che mostra la barra laterale,
  il focus passa a un controllo ancora visibile. Questo vale anche per il
  focus salvato della finestra inattiva e per il cambio di risoluzione.
- Una sequenza accodata Backspace/Invio nel campo Run svuota anche il campo
  disegnato. Lo Shift+Tab BIOS già accodato rimane un movimento all'indietro
  anche se Shift viene rilasciato prima che la UI elabori il tasto.
- Le brevi transizioni in modo protetto del renderer preservano FLAGS,
  GDTR, IDTR e il limite nascosto di ES. Una tabella temporanea valida in
  entrambi i modi rimanda gli NMI fino al ripristino del contesto originale;
  non vengono modificate le maschere NMI/CMOS. Sono usati salti far espliciti.
- Per VBE 3 vengono distinti passo delle righe, maschere colore e numero di
  pagine lineari da quelli a banchi. Un rifiuto dell'accesso lineare ripristina
  e rivalida i metadati del percorso a banchi. Le maschere colore con overflow
  sono respinte e il modo effettivamente attivo deve avere l'accesso richiesto.

## Prove mirate con controlli negativi

Evidenze sotto `build/full/ui-smooth-2026-09-25/`:

- `lfb-review/guard-qemu/results.json`: 35 casi QEMU Pentium III/TCG,
  inclusa iniezione di NMI a ciascun confine di istruzione della transizione,
  raffiche annidate, IVT spostata e limite nascosto ES ripristinato.
  `guard-negative/` conserva il triple fault della variante senza IDT
  temporanea: il test rileva realmente il guasto che deve prevenire.
- `vbe3-reviewed/results.json`: sette gruppi con descrittori lineari e a
  banchi deliberatamente differenti, rifiuti BIOS, maschere invalide e pulizia
  delle allocazioni. Il BIOS di questi test CPU è simulato esplicitamente.
- `claude-followup-cpu-fixed/results.json`: timer al cambio di mezzanotte,
  vere chiamate alle anteprime, Shift+Tab accodato e protezione del puntatore.
- `handoff-qemu-baseline-rerun/` e `handoff-qemu-candidate/`: confronto prima/
  dopo della finestra attiva al ritorno da DOS e prove di resize/Run. Per
  ottenere Backspace e Invio nella stessa elaborazione, il test riempie solo
  la coda BIOS della tastiera a VM ferma; non modifica stato UI o pixel.

## Problemi rilevati nei test finali

La suite del setup assumeva ancora tre controlli nel ciclo Tab della pagina
iniziale. Con Back disabilitato i controlli sono Next e Cancel: il test
selezionava Cancel prima di premere Invio. Le schermate
`install/before-page-1.png` e `install-debug/` conservano la diagnosi.
Il test ora verifica anche che il ciclo Tab ripristini visivamente il focus
iniziale, senza cambiare i criteri sui pixel esterni alle aree aggiornate.

Il test atomico a 2560×1440 ha invece trovato un fotogramma realmente parziale
(`max-2560/maximize-frame-000.png`): il conteggio statico delle pagine VBE era
zero, mentre la memoria e le scanline attive consentivano due superfici.
Il [sorgente SeaBIOS](https://github.com/coreboot/seabios/blob/master/vgasrc/vbe.c)
imposta proprio `linear_pages` a zero; la funzione 4F06 restituisce invece la
capacità disponibile. Il controllo statico è stato sostituito dalla verifica
della capacità attiva e del cambio di viewport con rilettura 4F07, conservando
i limiti di VRAM, scanline e indirizzamento. Il descrittore non viene falsificato.
Il nuovo gruppo CPU verifica sia l'accesso lineare sia quello a banchi con
conteggio zero, e respinge VRAM insufficiente, scanline insufficienti e viewport
rifiutati: 22 gruppi renderer superati complessivamente.

La stessa prova QEMU dopo la correzione (`max-2560-paged/result.json`) rileva
zero fotogrammi misti e zero pixel residui nel ciclo maximize/restore. I tempi
host misurati sono 54/53 ms, comprensivi di acquisizione e polling: non sono
benchmark di un Pentium III fisico.

## Build finale e prove integrate

La build finale è in `build/full/ui-smooth-release-2026-09-25/`.
HDD e CD contengono la stessa `SHELL.COM` di 59.280 byte, SHA-256:
`75fc39f18e1152e8ac843215d7383ef386d6d02c65f68b97db926c8d6948f274`.
Questo è anche l'eseguibile delle prove `max-2560-paged` e `handoff-paged`.

Prove integrate concluse sulla build finale:

- `native-800/result.json`: 128 MiB, finestre sovrapposte, drag/resize,
  minimizza/ripristina, bordi del cursore, DOS, Caps Lock, esecuzione COM e
  ritorno. Nessun pixel modificato fuori dal danno nei 32 frame campionati.
  Splash originale verificata, 11 stati della barra, avvio AC97 acquisito:
  1,254 s, RMS 3574, quindi audio effettivo non silenzioso.
- `native-2560/result.json`: la stessa suite completa a 2560×1440, sempre con
  128 MiB, passa con zero danno esterno in tutti i 32 frame di drag/resize.
  La prova atomica separata usa lo stesso identico eseguibile: nessun frame
  parziale nel maximize/restore campionato.
- `tests/native-cd.log`: avvio CD, controlli desktop, Run, comandi DOS,
  directory persistenti e configurazione video. I profili DOS 640/testo vengono
  salvati e realmente applicati alla console; il desktop normale conserva il
  minimo SVGA e usa AUTO. 800/1024 valgono per entrambi. Il disco di controllo
  rimane byte per byte invariato.
- `install.log`: setup a 800×600, navigazione senza modifiche fuori dalle aree
  previste, conferma obbligatoria, installazione e confronto di ogni settore
  con la sorgente, salvo la prevista patch D:→C:. Restart genera un evento
  QMP RESET richiesto dal guest; la stessa VM riparte dall'HDD senza CD ed
  esegue comandi DOS. Test su disco virtuale sacrificabile, non su hardware.
- `windows/result.json`: sulla nuova immagine HDD, due sessioni Windows 3.1
  con applicazioni reali, resize e repaint senza pixel residui, apertura/chiusura
  dei figli. WAV RMS 1752 e MIDI RMS 2431 acquisiti dall'SB16 emulata.
- `doom/results.json`: sull'HDD prodotto dal setup, Doom classico con titolo,
  attesa al menu, selezione episodio/difficoltà, gioco, movimento e uscita;
  esecuzione COM e mouse dopo il ritorno. Audio SB16 del gioco RMS 1940.
- `games/results.json`: sullo stesso HDD installato, Wolfenstein 3D e Doom
  Vanille raggiungono il gioco, rispondono al movimento e tornano al DOS con
  esecuzione COM funzionante. La stessa VM avvia poi Windows 3.1 e torna al
  DOS, verificando Caps Lock e COM. Audio AC97 dei giochi acquisito: RMS 569
  e 1114 rispettivamente; 128 MiB, nessuna sostituzione dei binari installati.
- `legacy-apps/results.json`: DOS Navigator legge la directory, cambia
  pannello e si chiude; Costa mostra il puntatore e risponde al movimento.
  Dopo entrambi viene eseguito un COM. Avvio dalla shell DOS su copia dell'HDD
  installato, che rimane invariato; stato di uscita del test 0.
- `gui-dosnav-checked/results.json`: sulla nuova immagine HDD, clic reale
  sul riquadro Files e Invio avviano DOS Navigator dal desktop. Tab cambia
  pannello; Alt+X e Invio escono e ripristinano il desktop. Seguono COMDEMO
  e un altro ritorno al desktop. QEMU rimane in esecuzione a ogni controllo
  e viene chiuso intenzionalmente dal test solo alla fine; stato 0.

Le prove di impostazioni complete e di salvataggio fallito sono conservate in
`ui-smooth-followup-2026-09-25/settings-final-{all,error}/`: anteprima,
annullamento, timeout di circa 12 s, conferma, file salvati, Windows INI
invariato e Play AC97 realmente acquisito. Usano il candidato precedente
all'ultima sola correzione del criterio di doppia pagina, non vengono spacciate
per una riesecuzione sulla release.

## Tracciabilità della build

Le prove intermedie restano in `build/full/ui-smooth-followup-2026-09-25/`;
immagini e report finali sono in `build/full/ui-smooth-release-2026-09-25/`.
`artifacts.json` contiene gli hash
delle immagini, `packaged-binaries.json` quelli dei file realmente estratti
dal FAT e `source-freeze.json` quelli dei sorgenti verificati.

Il wrapper di build salva e ripristina byte per byte le precedenti immagini
CD pubblicate; `build/full/obj` contiene invece gli oggetti dell'ultima build.
Nessun disco fisico viene formattato o masterizzato dai test.

La sessione interattiva ha usato la build finale a 128 MiB, AC97 e snapshot del
disco. `interactive.json` registra PID e immagine; `interactive-desktop.png`
mostra il desktop effettivo a 1280×800×32. La memoria della sessione VS Code
rimane limitata a 8.000.000.000 byte tramite il suo scope systemd.
`run-qemu.sh` nella directory della release riapre la stessa immagine in
snapshot con 128 MiB e AC97, senza ricompilarla.

La finestra della sessione manuale è risultata chiusa dopo l'avvio di DOS
Navigator. È stata chiesta conferma all'utente per sapere se l'abbia chiusa
manualmente; il solo log non permette di attribuirne la causa. Le successive
prove DOS e dal collegamento grafico non hanno riprodotto un crash: questo
limite della diagnosi resta distinto dai risultati automatici sopra riportati.
