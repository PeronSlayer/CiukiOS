# Aggiornamento desktop — 6 settembre 2026

Build: CiukiOS pre-Alpha 0.7.1. Le prove automatiche usano QEMU/KVM,
CPU Pentium III e 512 MB, salvo i gate preesistenti indicati nei log.
Non equivalgono a una verifica sul ThinkPad T23 fisico.

## Comandi e interfaccia

- Foto originale di Ciuki conservata. La barra segmentata segue il completamento
  di runtime, servizi, volume, shell e passaggio al prompt; rimossi i precedenti
  ritardi fissi della schermata di caricamento.
- Tema di avvio originale di 1,28 secondi: `SOUND TEST` per ascoltarlo,
  `SOUND OFF` per disattivarlo, `SOUND ON` per riattivarlo. Il driver ICH/AC97
  riproduce un buffer completo senza installare gestori IRQ e libera la memoria
  alla fine; sugli altri dispositivi rimane un breve motivo PC speaker.
- `HOME`: benvenuto a colori. `APPS` o F2: giochi, Windows, Costa, DOS Navigator,
  editor, schermo e anteprima sonora; frecce o numero, Invio, Esc.
- F1 / `HELP`: guida breve, con `HELP FILES`, `HELP NETWORK`, `HELP SYSTEM`.
- Tab completa e scorre i nomi della cartella/percorso corrente. Non cerca i
  comandi nel PATH. Frecce, Home/End, Canc e Backspace modificano la riga;
  Su/Giù e F3 richiamano la cronologia. `HISTORY` mostra gli ultimi otto comandi.
- `SYSINFO`, `MEM`, `DATE`, `TIME`: informazioni, memoria DOS libera e lettura
  dell'orologio. `WIN`, `DOOM`, `DOOMVAN`, `WOLF3D`, `DOSNAV` funzionano anche
  da altre cartelle, ripristinando la cartella del chiamante al ritorno.
- `VGASETUP`: selezione condivisa per shell e Windows; anteprima, conferma e
  ripristino automatico restano disponibili. I giochi che impostano direttamente
  la VGA conservano la propria risoluzione; all'uscita torna quella della shell.
  `VGASETUP SAFE` ripristina il profilo VGA Windows e la console testuale.

La preferenza sonora e quella video sono file sul volume DOS. Su un CD avviato
come RAM disk le modifiche durano fino al riavvio; su un volume scrivibile
persistono. Il CD di rilascio rimane **non compresso**, dopo l'errore MEMDISK/gzip
osservato sul T23. Non viene dichiarato risolto il tempo di copia BIOS del CD.

## Correzioni di compatibilità

Il difetto di ridimensionamento Windows è stato riprodotto nel profilo VBE
banked. Il solo timer di swap non lo risolve. I profili 800×600 e 1024×768 ora
preferiscono il framebuffer lineare e usano `SwapBuffersInterval=16` per
abilitare il doppio buffer. Restano profondità 8 bit, `BounceOnModeset=1` e
profilo VGA di recupero. La prova Win16 usa veri `MoveWindow`, ridimensionamenti,
minimizzazione/ripristino e `GetPixel`; non forza un repaint per nascondere il
problema. Il driver è la versione già presente: nessun aggiornamento binario
non verificato. La compatibilità del nuovo percorso lineare sul T23 va confermata.

I launcher Wolf3D e Windows riservano 1 KiB di stack. Con i precedenti
512 byte il launcher Wolf3D rientrava erroneamente nel bootstrap durante
l’avvio di HDPMI; l’avvio, l’audio e l’uscita passano con lo stack ampliato.

La console grafica della shell e di VGASETUP ora alloca separatamente i suoi
12 KiB di celle e li libera prima di EXEC. Un errore di cambio banco ripristina
il testo e lascia la liberazione al ritorno dal renderer.

Il caricatore MZ sceglie il più grande intervallo disponibile. `INT 21h/AH=4Ah`
aggiorna la dimensione effettiva del MCB, conservando il limite iniziale in
`PSP:2`, come nel DOS compatibile. Le ricerche di spazio, le catene di processi
e i TSR consultano la dimensione effettiva; non riutilizzano il vecchio limite
come memoria ancora occupata. Questo elimina l'errore di memoria insufficiente
riprodotto con DOS/32A. Il probe controlla sia il PSP sia il MCB e verifica che
un tentativo di crescita fallito non corrompa un'altra allocazione.

`COMMAND.COM /C programma` restituisce il codice d'uscita del figlio;
`COMMAND.COM /K comando` esegue il comando e resta interattivo. La shell
principale mantiene EXIT/QUIT disabilitati.

Doom classico conserva il launcher audio originale e il motore originale.
Il menu, la navigazione, una nuova partita e l'uscita passano in QEMU, con
segnale audio misurato. Il crash fisico riferito dall'utente non è ancora
attribuito con certezza: **non è dichiarato risolto sul T23**. Dalla cartella
`\APPS\DOOM`, `DOOMSAFE` offre un avvio diagnostico senza audio. Gli esperimenti
con un extender alternativo non sono inclusi nel rilascio.

## Windows 95 e limite ancora aperto

Windows 3.1 resta in Standard Mode per mantenere la gestione dell'audio
HDPMI/VSBHDA. Il prompt DOS interno avvia un'icona COMMAND ma non passa a una
console interattiva nei test: questo limite resta aperto. Per usare la shell
occorre uscire da Windows; il ritorno alla shell, il mouse e il successivo
avvio di Windows sono verificati.

Il lavoro su MCB/PSP, COMMAND e GDI prepara test più affidabili, ma non abilita
Windows 95. Windows 95 elimina Standard Mode e usa VMM/VxD: occorre prima
risolvere il passaggio alle applicazioni DOS, verificare l'istanziazione dello
stato DOS e gli ambienti EXEC, e progettare la convivenza o il passaggio di
proprietà di IRQ, audio e memoria al VMM. Il loader corrente non eredita ancora
ambienti EXEC arbitrari. Non sono stati installati Windows 95, alterate le
risposte di versione per simularne il supporto o cambiate partizioni fisiche.

## Prove e artefatti

Artefatti di questa sessione: `build/full/desktop-refresh-2026-09-06/`.

- `windows-release.log`: Windows 1024×768, audio WAV, CANYON.MID,
  puntatore, Alt+F4, uscita e secondo avvio; nella stessa sessione DOS Navigator,
  Doom e Costa, con risposta del mouse e tastiera.
- `windows-largest/`, `windows-psp/`, `windows-800-release/`: probe Win16 di conservazione dei pixel.
  `windows-before/` conserva la riproduzione del difetto.
- `doom-menu-final.log`, `doom-menu-final/audio.wav`: menu, nuova partita,
  tastiera, uscita e audio. Il test attende un frame completo prima di Esc:
  inviarlo durante il caricamento produceva falsi negativi.
- `shell-compat-final2.log`: file, percorsi, COM/MZ, processi annidati, TSR,
  stato di ritorno e shell principale.
- `shell-final-graphics.log`: editing, completamento anche in cartella vuota,
  righe lunghe, cronologia, F1/F2 e preferenza sonora.
- `dos-memory-release.log`: PSP/MCB, isolamento delle allocazioni, codice 37
  restituito attraverso COMMAND /C e permanenza interattiva con /K.
- `boot-release.log`: barra animata, tastiera al prompt, chime misurato e
  riavvio da disco con preferenza OFF, senza emissione PCM.
- `doom-safe-final.log`: avvio diagnostico, menu, partita e uscita.
- `wolf-release.log`: gioco, frame 640×400, audio AC97 e ritorno alla shell.
- `doomvan-final.log`: partita, grafica, audio AC97 e uscita di Doom Vanille.
- `ps2-final.log`: codice macchina PS/2, arrivo simultaneo di tasti/pacchetti,
  mouse assente, risposte errate, timeout e IRQ12 spurio.

Le prove fallite di sviluppo restano distinguibili dai gate superati; in
particolare `windows-*-prompt`/`windows-trace` non sono test riusciti del prompt.

La compilazione VSBHDA è serializzata: i due target a 16 bit aggiornano
librerie OpenWatcom condivise, che la compilazione parallela poteva corrompere.
Una nuova compilazione del componente a 16 bit non era identica a quella
verificata e il successivo gate Windows→Doom è fallito
(`cd-audio-windows.log`, immagine scartata). Il rilascio usa quindi **gli esatti
componenti audio estratti dalla HDD già verificata**, incluse licenze e driver,
attraverso la modalità `CIUKIOS_SBEMU_MODE=reuse` esistente. La corrispondenza
binaria di tutti i componenti è verificata prima della masterizzazione.
La causa della differenza fra le compilazioni resta da approfondire.

Comando di questa release:

```sh
CIUKIOS_SBEMU_MODE=reuse \
CIUKIOS_SBEMU_OUTPUT_DIR="$PWD/build/full/desktop-refresh-2026-09-06/tested-audio" \
bash scripts/build_full_cd.sh
```

## ISO e supporto ottico

- File: `build/full/CiukiOS_full_cd_0-7-1.iso`.
- Dimensione: 101.261.312 byte.
- SHA-256 locale: `30bb97059a3eca8c13e3fae3bc6f389f6611fa1eb0643dd37f55ab9c9076a032`.
- Configurazione: `harddisk raw`, immagine RAM non compressa.
- `cd-boot-tested.log`: prova della ISO effettiva con 512 MB.
- `cd-doom-tested.log`: dalla stessa ISO, menu, partita, tastiera e uscita di
  Doom; audio misurato dopo la fine del chime, RMS 1621,96 e picco 21334.
- Masterizzazione completata sul TSSTcorp TS-H653N (`/dev/sr0`):
  `burn-retry.log`, xorriso termina con codice 0 e conferma
  `Writing to '/dev/sr0' completed successfully.` Il CD-RW è chiuso;
  comando eseguito con espulsione finale (`-eject`).
  Il primo tentativo (`burn.log`) non aveva rilevato il supporto.
  Non sono stati eseguiti controlli separati del disco o riletture di verifica.

## Fonti consultate

La ricerca sulle preferenze è qualitativa, non un sondaggio. Ho scelto funzioni
coerenti con FreeCOM e richieste di accesso rapido ai giochi DOS: cronologia,
completamento, un menu utilizzabile con frecce e Invio e un gestore file già
presente. Non è stato sostituito DOS Navigator.

- [FreeCOM — sorgenti e funzioni](https://github.com/FDOS/freecom).
- [Discussione DOS gaming su launcher accessibili](https://www.reddit.com/r/dosgaming/comments/1jiyx5y/).
- [VBESVGA — configurazione del framebuffer e doppio buffer](https://github.com/PluMGMK/vbesvga.drv).
- [FreeDOS — DosMemChange, ridimensionamento del MCB](https://github.com/FDOS/kernel/blob/master/kernel/memmgr.c).
- [Riferimento MS-DOS — caricamento degli eseguibili](https://www.pcjs.org/documents/books/mspl13/msdos/dosref33/).
- [Microsoft — Standard Mode, VMM e passaggio a Windows 95](https://learn.microsoft.com/en-us/archive/msdn-magazine/2000/july/under-the-hood-happy-10th-anniversary-windows).
