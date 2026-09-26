# T23: mouse Costa, ritorno COM e anteprima VBE

Riscontro successivo alla masterizzazione qui descritta: tastiera bloccata
e caricamento del CD lento. Correzione e nuova ISO sono documentate in
[CD: tastiera e avvio](cd-keyboard-boot-2026-09-05.md).

## Riscontro dell'utente

- Windows: audio e MIDI funzionanti.
- DoomVan e Wolf3D: funzionanti, audio compreso.
- DOS Navigator: funzionante.
- Doom originale: non parte più; comando e schermata d'errore non disponibili.
- Costa: desktop disponibile, cursore fermo.
- VGASETUP: riavvio durante il cambio modalità e all'avvio di Windows dopo
  la selezione di una modalità video superiore.

## Difetti riprodotti e modifiche

### Mouse nella distribuzione CD

`build_full_cd.sh` impostava `CIUKIOS_ENABLE_PS2_MOUSE_INIT=0`, mentre la build
HDD usata dal test Costa impostava `1`. Anche il kernel installato da quel CD
conservava il valore disabilitato. La vecchia ISO è stata avviata direttamente
in QEMU: Costa e Calculator si aprivano, ma il movimento PS/2 produceva **zero
pixel modificati** nel confronto del cursore.

Il CD ora usa lo stesso valore predefinito `1` della build HDD. Non sono stati
modificati il parser dei pacchetti, accelerazione, callback, cursore o driver
Windows. Il test Costa accetta `COSTA_BOOT_ISO` per verificare il vero avvio
Live CD, usando `IMG` solo per controllare il contenuto della partizione FAT.

Un secondo caso è stato riprodotto dopo due sessioni Windows: il test esteso
raggiungeva Costa, ma il confronto prima/dopo il movimento contava nuovamente
zero pixel. `INT 15h/C200h` disabilitava l'invio dei pacchetti con F5; il reset
INT 33h del successivo client DOS ripristinava solo lo stato software.

Il kernel ora registra lo stop fisico richiesto dal BIOS (F5, F6 o reset) e,
solo dopo la fine dell'uso BIOS, il reset INT 33h invia F4 per riattivare il
flusso. I reset DOS normali non aggiungono comandi al dispositivo e un client
BIOS ancora attivo conserva il controllo. Il flag fisico non fa parte delle
istantanee del processo. Pacchetti, sensibilità e callback non cambiano.

### Ritorno normale dei programmi COM

`int21_exec_run_com` entrava nel programma con una `CALL FAR` dal kernel.
Il normale `RET` finale di un COM prendeva quindi dallo stack un offset del
kernel e lo eseguiva nel segmento del programma, invece di tornare a
`PSP:0000 / INT 20h`. MODETEST termina proprio con `RET`: prima della correzione
l'anteprima 800x600 si apriva, ma l'uscita perdeva la shell e rieseguiva codice
di avvio. Sono stati conservati schermata e registri del blocco.

L'ingresso usa ora un salto FAR con `PSP:0000` preparato sullo stack. La
profondità iniziale dello stack è identica a quella precedente. `RET` e i
vecchi esempi `RETF` terminano entrambi tramite INT 20h e il normale recupero
di processo, DTA, vettori e memoria. Le API mouse e audio restano invariate.

### Errori dell'anteprima VBE

Il MODETEST upstream non controllava il risultato di `4F02h` prima di scrivere
nel framebuffer. Per cambiare banco chiamava direttamente `WinFuncPtr`, senza
impostare AX, e riutilizzava DX dopo la chiamata.

La patch del sorgente controlla il modeset, usa `INT 10h / AX=4F05h` per i
banchi in modalità reale e controlla anche questo risultato. Conserva il
numero di banco prima della chiamata. Un errore riporta la modalità testo e
un messaggio, senza proseguire a disegnare. Il test LFB conserva il proprio
`bankemu`, che non chiama il BIOS in modalità protetta.

Il builder verifica prima la corrispondenza byte per byte dei cinque helper
upstream con la release fissata, poi ricompila MODETEST dalla copia patchata.
I driver Win16 e i sorgenti upstream nel checkout non vengono alterati.

Riferimenti:
[MODETEST upstream](https://github.com/PluMGMK/vbesvga.drv/blob/afaacaef6b7036e4d8a8b947244e2f4a6c209564/MODETEST.ASM),
[interfaccia VBE](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf).

## Verifiche

Artefatti della sessione: `build/full/t23-video-mouse-doom-2026-09-05/`.

- Vecchia ISO / Costa: FAIL, cursore immobile riprodotto.
- Anteprima VBE prima della correzione del caricatore: FAIL al ritorno.
- Caricatore finale: PASS dei COM con `RET` e `RETF`.
- Anteprima finale: PASS 800x600 e ritorno alla shell; PASS per rifiuto del
  modeset e del banco introdotti da un TSR di test separato dal prodotto.
- Controlli VGASETUP: PASS stato visibile, persistenza 80x50, profili Windows
  e riavvio controllato in QEMU.
- Doom originale a freddo, prima delle modifiche: PASS gameplay, audio e uscita.
- Windows 800x600: PASS desktop, mouse, WAV/MIDI, uscita e secondo avvio in QEMU.
- Build finale Windows 1024x768: PASS desktop, puntatore singolo, WAV/MIDI,
  uscita, DOS Navigator e controllo dello stato DOS, secondo avvio Windows,
  Doom originale in gameplay e uscita, poi Costa e Calculator nello stesso
  avvio. Il movimento del cursore Costa modifica 52 pixel.
- DoomVan e Wolf3D finali: PASS gameplay visibile, audio AC97 e uscita alla
  shell funzionante. RMS della componente AC rispettivamente 524,01 e 487,68.
- ISO finale avviata direttamente da CD virtuale: PASS desktop Costa,
  cursore (52 pixel modificati) e Calculator a 640x350.
- Shell e processi COM annidati: PASS, compreso il ripristino dello stato
  della shell principale.

I log finali sono `hardware-controls-all-fixes.log`,
`windows-1024-all-fixes.log`, `doomvan-final-test.log`,
`wolf3d-final-test.log`, `costa-cd-all-fixes-test.log` e `shell-final.log`.
La cattura `second.png` del test Windows precedeva l'attesa del desktop e
mostra il frame iniziale nero; il secondo avvio è seguito dall'uscita
controllata e dalle prove DOS. Lo script ora ricattura quel frame dopo
l'attesa, per rendere più utile l'artefatto delle prossime esecuzioni.

Questi risultati non dimostrano che il BIOS SuperSavage del T23 sia stabile
in tutte le modalità Windows. Il mancato avvio del Doom originale riferito
dall'utente non è ancora attribuito a una causa dimostrata. Una prova del suo
motore attraverso DOS4GW esterno è stata scartata: il formato dell'eseguibile
originale non è accettato da quel loader. Motore e launcher originali restano
quelli già distribuiti, salvo gli effetti della correzione generale dei COM.

## RAM della workstation

Limite attivo per questa sessione e tutti i suoi figli:
`MemoryMax=8000000000`, `MemoryHigh=7000000000`.
Impostazione runtime con systemd; non cambia i limiti delle altre sessioni
VS Code e non persiste dopo la chiusura di questa istanza.
Applicata inizialmente ad `app-code-32478.scope`; dopo la ricreazione della
sessione durante il lavoro è stata riapplicata ad `app-code-79264.scope`,
verificato attraverso `/proc/self/cgroup`.

## Supporto ottico

La vecchia ISO è conservata in `before.iso` nella cartella degli artefatti.
ISO finale: `build/full/CiukiOS_full_cd_0-7-1.iso`, 101261312 byte.
SHA-256: `a74e1c8fabfb6a0021428e693fd5f3211421aad2097c16a9db83dd813748a07e`.
CD-RW in `/dev/sr0` masterizzato con successo ed espulso. Comando eseguito:

```sh
xorriso -as cdrecord -v dev=/dev/sr0 blank=fast -dao -eject build/full/CiukiOS_full_cd_0-7-1.iso
```

Uscita del processo: `0`. Log: `burn.log` nella cartella degli artefatti.
Nessun controllo preliminare separato del supporto e nessuna rilettura di
verifica, come richiesto dall'utente; solo l'inizializzazione del dispositivo
necessaria al programma di masterizzazione.
