# Fluidità della UI: framebuffer lineare e presentazione a doppia pagina — 25 settembre 2026

Per le correzioni emerse nell'ultima revisione, la build finale e le prove
integrate successive, vedere
[Chiusura della revisione UI di Claude](claude-followup-validation-2026-09-25.md).

Obiettivo: eliminare l'effetto "scansione" (ogni movimento ridisegnato a fasce,
dall'alto verso il basso, con ritardi visibili) nella UI nativa di SHELL.COM, nel
Setup grafico e in VGASETUP. Le misure QEMU/KVM qui riportate non sono misure di
un Pentium III o di una ATI Radeon 9200 fisici.

## Diagnosi misurata

1. **Ogni cambio di banco VBE costava circa 6 ms in QEMU/KVM.** Tracciando
   `vga_vbe_write` e `kvm_set_user_memory` durante un maximize, ogni
   `INT 10h/4F05` produceva due aggiornamenti di memslot KVM (rimozione e
   ricreazione dell'alias `A0000`), ciascuno di circa 3 ms. Il renderer scriveva
   solo attraverso la finestra a banchi da 64 KiB:
   - un semplice movimento del puntatore faceva 4–6 cambi di banco (salvataggio,
     contorno nero e riempimento bianco disegnati in tre passate separate, pixel
     per pixel), cioè 30–40 ms per movimento;
   - maximize a 1280×800×32: 83 cambi di banco, ~575 ms;
   - maximize a 2560×1440×32: ~1,65 s (2,3 s nella misura del test di regressione).
2. **La doppia pagina era sempre spenta con i profili espliciti** (`0800`,
   `1024`, cioè le scelte 800×600/1024×768 del pannello Display). Solo `vc_auto`
   leggeva le informazioni del controller VBE (`4F00`); con un profilo esplicito
   `TotalMemory` restava 0 e `ui_pages_begin` rifiutava la seconda pagina. Ogni
   ridisegno veniva allora presentato a fasce sulla pagina visibile: nella suite
   delle finestre a 800×600, **10 frame di trascinamento su 24 campionati erano
   disegnati a metà**. Tre revisori indipendenti hanno trovato lo stesso difetto.

## Soluzione

### Backend framebuffer comune (`src/com/vbe_fb.inc`)

Tutti gli accessi al framebuffer (compositor, puntatore, console testuale,
clear, span e pixel singoli, Setup, VGASETUP) passano ora da poche primitive:
`vc_fb_put`/`vc_fb_get` (copia memoria↔framebuffer), `vc_fb_fill`,
`vc_fb_put_rows`/`vc_fb_get_rows`, `vc_pointer_save/draw/restore`.

- Se il BIOS dichiara un framebuffer lineare (attributo bit 7 e `PhysBasePtr`)
  e la CPU è in modo reale (non V86), il modo viene impostato con il bit `0x4000`
  e ogni accesso è una **raffica breve**: `vc_fb_begin` disabilita gli
  interrupt, entra in modo protetto per poche istruzioni per dare a ES un limite
  piatto di 4 GiB e torna in modo reale; la copia usa istruzioni stringa a 32 bit.
  `vc_fb_end` ricarica allo stesso modo un limite di 64 KiB e ripristina ES,
  GDTR e FLAGS. **Nessun limite "unreal" sopravvive a una raffica**: i programmi
  DOS figli non ne ereditano mai uno. Le raffiche sono annidabili e limitate a
  una scanline, una riga di celle o il puntatore, quindi la latenza degli
  interrupt resta breve.
- A20 viene abilitato con la richiesta *local enable* del gestore XMS residente
  (bilanciata da *local disable* in `vc_end`) e verificato con un test di
  wrap-around; senza A20 o in modo V86 si usa il percorso a banchi.
- Le copie verso il framebuffer lineare sono verificate contro la superficie
  validata (`vc_access_bytes`): nessuna scrittura fuori dalle pagine note.
- Il percorso a banchi resta come fallback, con le stesse primitive; la console
  testuale conserva l'ordine per scanline che minimizza i cambi di banco.
- I modi solo-LFB (senza finestra a banchi) sono ora accettati quando l'accesso
  lineare è possibile.

### Presentazione

- Le informazioni del controller VBE vengono lette una volta anche con i profili
  espliciti (`vc_controller_query`): la doppia pagina si attiva a 800×600 e
  1024×768 e la validazione della VRAM torna attiva.
- Il puntatore viene salvato, composto in memoria e scritto una riga per volta
  in un'unica raffica (prima: tre passate pixel per pixel).
- A riposo, la pagina nascosta viene allineata al danno appena presentato
  (`ui_pages_sync`): il frame successivo compone solo il proprio danno invece
  dell'unione con quello precedente (es. il primo frame dopo un maximize o dopo
  il ritorno da un gioco).
- Dopo il cambio di modo il BIOS ha già azzerato la memoria video: il secondo
  azzeramento software è stato rimosso (fino a 14 MiB a 2K).
- La taskbar viene ridisegnata solo quando un pulsante di task compare o
  scompare, non a ogni cambio di focus; l'animazione di focus non gira per una
  finestra invisibile.
- Il marcatore seriale `[DESKTOP] PAINT` riempie la FIFO 16550 invece di
  attendere che si svuoti prima di ogni carattere (circa 4 ms per frame a
  38400 baud su hardware reale).
- Il puntatore resta visibile durante il trascinamento.
- Al ritorno da un programma avviato dal desktop non viene più ricostruita (e
  subito distrutta) la console VBE: due cambi di modo e un azzeramento in meno,
  cioè due risincronizzazioni del monitor in meno.

## Altri difetti corretti

Trovati da due giri di revisione con verifica avversariale indipendente:

- VGASETUP: il testo dell'anteprima restava in coda nella console a lotti e non
  veniva mai disegnato (anteprima nera per 12 s). Ora viene svuotata la coda; il
  percorso `DESKTOP` controlla anche `vc_active`.
- VGASETUP `DESKTOP 800/1024` mostrava in anteprima il modo più profondo di quella
  geometria ma salvava un profilo che la shell traduce nei modi 0x103/0x105: ora
  l'anteprima usa esattamente il modo che verrà usato.
- VGASETUP: annullamento o timeout dell'anteprima escono con codice 5
  ("nulla salvato"); la shell mantiene la modalità SAFE e non mostra errori.
  Prima un annullamento azzerava la modalità SAFE.
- VGASETUP legge i tick dal BDA invece di `INT 1Ah/00h`, che consumava il flag
  di mezzanotte usato da DOS per avanzare la data; il menu imposta il modo
  testo una sola volta (niente sfarfallio a ogni tasto) e il messaggio
  "Preview cancelled" resta leggibile.
- Setup: dopo un errore o un annullamento, "Try again" conservava la conferma di
  cancellazione del disco precedente; su un altro disco bastava Invio per
  cancellarlo senza nuova conferma. Ora la conferma viene azzerata.
- Setup: nel fallback VGA la pagina di avanzamento mostrava il puntatore due
  volte e lasciava una freccia fantasma; Tab portava il focus su pulsanti
  inesistenti delle pagine finale/errore; il renderer rifiuta un modo 0x103 che
  non sia 800×600×8; il puntatore viene nascosto una sola volta per ridisegno e
  solo se interseca l'area modificata; la presentazione non rilegge più la VRAM.
- Shell: Alt+Tab dopo la chiusura/minimizzazione della finestra in primo piano
  non faceva nulla al primo tentativo; Shift+Tab andava avanti invece che
  indietro; Tab si fermava sulle icone della libreria, che non mostrano il focus;
  con BlocNum attivo i tasti del tastierino nel campo Run spostavano il cursore
  o cancellavano invece di scrivere cifre e punto; il menu CiukiOS restava aperto
  dopo la scelta; il mouse era disattivato se il driver riportava una dimensione
  di stato 0 o superiore a 512 byte; la maniglia di ridimensionamento della
  finestra Programs poteva finire fuori dalla portata del puntatore; i comandi
  esterni preceduti da spazi davano "command: not found"; la prima riga di
  output di un comando di testo veniva cancellata dalla barra del titolo.

## Misure (QEMU/KVM, Pentium III, 128 MiB)

| Operazione | Prima | Dopo |
| --- | --- | --- |
| Maximize 1280×800×32 (click → PAINT) | 575 ms | ~34 ms* |
| Maximize/restore 2560×1440×32 (test di regressione) | 2,30 / 2,36 s | 0,045 / 0,047 s |
| Click su un controllo, rilascio → PAINT | ~169 ms | ~32 ms* |
| Cambi di banco per un movimento del puntatore | 4–6 | 0 |
| Frame di drag a metà (800×600, 24 campioni) | 10 | 0 |
| Mouse dopo Doom (soglia 300 ms) | fallito (~777 ms) | superato |

\* Il valore include il polling del log seriale del test (~30 ms): è il limite
di risoluzione della misura, non il tempo di disegno del guest.

## Prove eseguite

Evidenze in `build/full/ui-smooth-2026-09-25/` (immagine completa, ISO,
log di build e test).

- `uv run --with unicorn python scripts/test_ui_rendering.py`: 21 gruppi
  superati. Ai 14 esistenti (percorso a banchi) si aggiungono 7 gruppi per il
  percorso lineare: modi solo-LFB e rifiuto del wrap a 4 GiB, rifiuto con A20
  disabilitato, band presentate byte per byte identiche al percorso a banchi a
  5 profondità senza cambi di banco né letture VRAM, span/copie/clear entro la
  superficie, console a lotti e diretta identica al percorso a banchi con
  verifica indipendente del glifo, puntatore salva/disegna/ripristina all'angolo
  dello schermo a 5 profondità, doppia pagina che presenta solo scene complete.
  Ogni chiamata lineare verifica il ripristino di GDTR, IF, ES e GS, il
  bilanciamento delle raffiche e l'assenza di scritture nella tabella dei
  vettori di interrupt.
- `qemu_test_native_windows.py` a 800×600 (profilo `0800`), 1024×768, AUTO
  (1280×800) e 2560×1440: suite completa superata (sovrapposizioni, drag,
  resize, minimizza/ripristina, cursore ai quattro bordi, DOS, Caps Lock,
  COMDEMO e ritorno).
- `qemu_test_ui_regressions.py`: selection, focus, release, idle,
  keyboard-minimize, maximize atomico a 800×600 e a 2560×1440: tutti superati.
- `qemu_test_full_shell_desktop.py --graphics` (console grafica),
  `qemu_test_full_display_profile.py` (profili `0800` e `TEXT`),
  `qemu_test_desktop_modes.py` (anche con VRAM limitata),
  `qemu_test_vgasetup_entry.py` (normale e anteprima rifiutata),
  `qemu_test_full_desktop_refresh.py --case doom`: superati.
- `qemu_test_installed_hdd.py --case classic-doom --audio sb16 --ui-palette
  platinum --memory 128` con la nuova SHELL.COM sull'immagine installata della
  diagnosi precedente: superato, compreso il controllo del mouse a 300 ms dopo
  Doom che era fallito nella candidate-5.

### Correzioni dei test (con prova)

- `qemu_test_native_windows.py` consentiva per i dialoghi un'area di 470×230
  pixel, ma finestra e ombra occupano 471×232 (lo stesso rettangolo usato dal
  danno di produzione); per il resize ometteva l'ultima riga d'ombra (`wy+5`) e
  il puntatore nella posizione finale. Lo stato finale della **baseline
  invariata** ha esattamente gli stessi 468/683 pixel "fuori area": il test
  passava solo perché il disegno lento a banchi non terminava entro i 4–8
  campioni. I fallimenti sono conservati in `tests/` e nello scratch della
  sessione.
- `qemu_test_native_desktop.py` e `qemu_test_setup_graphical.py` cercavano il
  cursore con i colori della palette precedente al tema platinum: fallivano
  identici anche sulla ISO di sviluppo invariata, con la freccia intatta a
  schermo. Ora riconoscono entrambe le palette.

## Limiti e punti aperti

- Nessuna prova su hardware fisico (ThinkPad T23, ATI Radeon 9200): il percorso
  lineare usa solo funzioni VBE 2.0 standard, ma va verificato. Il framebuffer
  lineare resta *uncached*: impostare un MTRR write-combining accelererebbe molto
  le scritture su un Pentium III, ma non è stato fatto.
- Il percorso VGA planare (modalità SAFE) e il fallback senza buffer di
  composizione ridisegnano ancora l'intera scena direttamente a schermo.
- I click premuti e rilasciati durante un ridisegno lungo possono ancora andare
  persi (ora i ridisegni durano millisecondi in QEMU).
- `thinkpad_detect` in VGASETUP abilita scritture EC/CMOS se la stringa "IBM"
  compare in un punto qualsiasi del BIOS: rischio su portatili non IBM, non
  modificato senza hardware su cui verificare.
- `qemu_test_desktop_capabilities.py` (`depths`, `no-edid`) fallisce identico
  sulla baseline: si aspetta 640×480 senza EDID e cerca i colori della palette
  precedente. Non aggiornato.
