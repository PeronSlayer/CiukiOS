# Immagine T23 6c3b7800…: correzioni della prova 5bb73dba e linea Savage completa

- **Data:** 2026-10-27
- **Tipo:** modifica, integrazione
- **Versione/commit:** 1.0.1 alpha, main `21fa0e975` (+ `2ff750363`, solo hash delle DLL Wine); immagine T23 `6c3b7800d6062c304872748fd7dd77b06bf836ef6a3f62e0ed6fe62941051b5e`, `ciukios.img` `83c745f2…`, ISO di Setup `dac562d5…`
- **Stato:** completato (prova T23 del proprietario da fare)

## Contesto

Correzioni dei problemi della prova [2026-10-26-03](2026-10-26-03-prova-t23-5bb73dba.md)
e chiusura del percorso 3D Savage, come deciso dal proprietario.

## Cosa cambia

- **VM BIOS e modalità video (fx-biosvm).** Le porte di ritardo 0x80/0xED
  sono innocue nella VM BIOS: la connessione APM del T23 (F000:462D) non
  la mette più in quarantena. La quarantena è per famiglia di servizi BIOS,
  quindi un guasto APM non disattiva il VBE. Un lease per la modalità già a
  schermo non reimposta la modalità. Sui pannelli piatti la modalità passa
  prima dalla chiamata BIOS VBE (pratica `UseBIOS` di X.Org), quella nativa
  solo dove è provata su quella testa: corregge l'immagine alterata di
  Warcraft II a schermo intero. La tavolozza segue la larghezza del DAC
  lasciata dal BIOS (6 bit dopo un mode set VBE, 8 bit verificati con 4F08).
- **Spegnimento (fx-acpioff).** Alternativa ACPI S5 quando il soft-off APM
  manca o ritorna: lettura limitata del pacchetto `\_S5` in DSDT/SSDT,
  `SLP_TYP|SLP_EN` su PM1a/PM1b dopo la barriera dei dispositivi,
  abilitazione ACPI via `SMI_CMD` solo se serve, restituzione al firmware in
  caso di errore. Senza alcun provider la schermata "puoi spegnere" non
  mostra più un codice d'errore. 349 casi host AML/FADT.
- **Quake III (fx-q3hang).** Lease "portatore" da `ChangeDisplaySettings`
  ceduto solo alla finestra di gioco della dimensione della modalità; le
  altre finestre dello stesso processo tornano alla composizione; il
  rilascio di un lease preso in prestito revoca quello del portatore;
  `DISP_CHANGE_FAILED` per i guasti del driver; `WaitForInputIdle` segue la
  presenza reale della coda dei messaggi e restituisce
  `ERROR_NOT_GUI_PROCESS` come documentato da Microsoft, così l'installer
  VISE non resta più in attesa infinita.
- **Cursore di avvio (fx-launchfb).** Cursore "avvio in corso" per ogni
  avvio (desktop, Start, Esegui, File, collegamenti; nativo, Win32, DOS)
  fino alla prima finestra presentata, a un errore, all'uscita o al timeout
  di 5 s; `STARTF_FORCEONFEEDBACK`/`FORCEOFFFEEDBACK` rispettati.
- **Giochi DOS in finestra (fx-doswin3, fx-dospic, fx-vmclock).** CLI/STI
  snelli, flush del TLB solo per le voci usate, presentatore guidato dai
  flip con attesa limitata, presentazione diretta di una finestra DOS non
  coperta. IRQ0 e IRQ tracciati trattenuti finché l'handler del guest non
  fa IRET (modello VPICD di Windows 95). Nel replay delle misure T23 il
  rapporto tra tempo reale e tempo di gioco scende da 3,85 a 1,69 per DOOM
  e da 5,46 a 1,81 per Wolfenstein 3-D. Il modello IRQ dell'audio torna sotto
  il budget di 250 µs (da 252 a 245 µs).
- **Mouse DOS (fx-dosmouse).** INT 33h ripristinato (FreeDOS lo aveva
  sostituito con un IRET), callback in modalità protetta tramite l'host
  DPMI, mappatura delle coordinate in finestra e a schermo intero, funzioni
  15h/16h/17h; in finestra il mouse arriva al gioco dopo un clic di cattura.
- **DOOM a schermo intero (fx-doomfs2).** Uscite snelle per le porte
  OPL/SB16 anche a schermo intero nativo: costo modellato delle uscite
  calde −66 %, tick di gioco e audio stabili con molti effetti.
- **Duke Nukem 3D (fx-duke3d).** Shareware 1.3D preinstallato: archivio
  ufficiale con SHA-256 fissato, licenza shareware installata con il gioco,
  voce nel menu Giochi, avvio con DOS/4GW, Sound Blaster 16 preimpostata.
- **NetSurf (fx-nsmodern).** Colori CSS moderni, proprietà personalizzate
  (`var()` con fallback ed ereditarietà, budget di memoria per documento,
  rilevazione dei cicli), `color-mix()` in sRGB, `gap` nei flexbox, script
  modulo ignorati come in un browser ES5.
- **Savage 3D completo (f7-12, f7-13, f7-14, f7-15).**
  - Formati accelerati come predefiniti: D24/S8 e RGB565.
  - Qualificazione in background con budget (5 s totali), fuori dal
    percorso di avvio; corretto il riconoscimento di `3d=probe` nel loader
    (causa di `L:SELECT_ERROR`).
  - VRAM ceduta dalla cache 2D ai contesti 3D; qualificazione conservata
    tra i cambi di modalità con una breve rivalidazione.
  - Target di rendering residenti in VRAM, senza readback per fotogramma.
  - Scanout diretto della GPU per i lease a schermo intero; clear di
    profondità e stencil mascherati dal motore.
  - OpenGL nativo (libgl dell'SDK e Graphics Demo) sullo stesso motore
    hardware.
- **Correzioni dei banchi di prova.** Il frame di `records()` in
  Installazione applicazioni torna sotto 4096 byte; le build fwqueue
  collegano `apm.c`; regex della memoria DOS per Wolfenstein 3-D.

## Verifiche

`make sdk`, `make build-full` (T1: 2239 payload riletti con dimensione e
SHA-256 coincidenti; ISO di 236774 settori), smoke QEMU fino al desktop:
verdi. Suite kernel host verde su main `6f5d5a6d7`; test host di Quake III
(lease, installer, menu reale sull'host H1 a 25,4 fotogrammi/s), desktop v3,
user32, presentazione e ABI (3121 controlli) verdi sul ramo fx-q3hang dopo il
merge di main. Immagine hardware con `safe=0 serial=1 3d=probe`.
