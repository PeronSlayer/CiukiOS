# Avvio T23 e risoluzione condivisa — 6 settembre 2026

La build ripristina il caricamento MEMDISK non compresso e aggiunge un menu
VGASETUP che applica la stessa risoluzione alla shell CiukiOS e a Windows.

## Avvio dal CD

La foto del T23 mostra un arresto prima dell'avvio di CiukiOS: ISOLINUX carica
il file `.img.gz`, poi MEMDISK termina con
`Decompression error: invalid compressed format (err=1)`.
L'immagine compressa supera i test in QEMU ma non l'avvio su questa macchina.
La causa interna del guasto MEMDISK non è stata dimostrata.

La ISO distribuita torna quindi al disco `.img` non compresso, con
`APPEND harddisk raw`. Questo elimina il passaggio di decompressione che si
blocca nella foto. La lettura iniziale resta di circa 96 MiB e può richiedere
tempo sul lettore del T23. L'opzione `CIUKIOS_FULL_CD_COMPRESS=1` rimane soltanto
un esperimento esplicito, disabilitato nella build predefinita.

Restano incluse le correzioni alla separazione dei dati PS/2 di tastiera e
mouse descritte nelle [note precedenti](cd-keyboard-boot-2026-09-05.md).
Non sono stati modificati nuovamente kernel, driver audio o programmi DOS per
aggiungere la console grafica.

## Uso di VGASETUP

Digitare `VGASETUP`, scegliere con le frecce o con i tasti 1–4 e premere Invio.
Una scelta grafica apre un'anteprima nella risoluzione selezionata: Invio
conferma, Esc annulla, oppure dopo circa 12 secondi si ritorna al menu senza
salvare. Un cambio modalità o banco video rifiutato dal BIOS torna alla
schermata testuale. La scelta di recupero VGA si applica direttamente.

| Scelta | Shell CiukiOS | Windows |
| --- | --- | --- |
| 640×480 | 80 colonne × 30 righe, inclusa intestazione | 640×480, VGA 16 colori |
| 800×600 | 100 colonne × 37 righe, inclusa intestazione | 800×600, VBESVGA |
| 1024×768 | 128 colonne × 48 righe, inclusa intestazione | 1024×768, VBESVGA |
| Recupero VGA | Testo VGA nativo | Profilo VGA sicuro |

La risoluzione è condivisa; profondità colore e dimensione dei caratteri
dipendono dall'ambiente. La shell usa un font ROM 8×16 e modalità VBE con
finestra video, senza richiedere un framebuffer lineare.

Sono disponibili anche `VGASETUP SET 640`, `SET 800`, `SET 1024` e `SET SAFE`.
Questi comandi espliciti applicano la scelta senza anteprima. Gli alias
precedenti `VGASETUP WIN ...` aggiornano ora anche la risoluzione della shell.
`VGASETUP STATUS` mantiene la consultazione dello stato.

Il salvataggio aggiorna insieme `C:\SYSTEM\VIDEO\DISPLAY.CFG` e
`C:\WINDOWS\SYSTEM.INI`, usando file temporanei e copie di recupero. Se il
secondo aggiornamento fallisce, viene ripristinato il profilo Windows
precedente. I percorsi riservati alla transazione non possono sostituire
directory o file di sola lettura.

Su un sistema installato, la scelta rimane dopo il riavvio. Dal Live CD viene
salvata nel disco RAM e si perde allo spegnimento; il supporto ottico non viene
riscritto. I programmi DOS che impostano direttamente VGA, come i giochi,
usano la propria modalità durante l'esecuzione. Al loro ritorno la shell
ripristina la risoluzione scelta e recupera l'output testuale disponibile.

## Verifiche

Le prove sono in QEMU; il T23 fisico richiede il nuovo avvio dal CD.

- `scripts/qemu_test_full_display_profile.py`: avvio e tastiera, anteprima
  640×480 annullata, timeout senza salvataggio, conferma interattiva 800×600,
  scorrimento e CLS oltre i confini dei banchi video, ritorno da un programma
  in modalità 13h, persistenza 1024×768 dopo riavvio, transazione fallita con
  ripristino e recupero VGA. Tutto superato.
- `scripts/qemu_test_full_windows31.sh`: shell e Windows entrambi a 1024×768,
  mouse, audio Sound Blaster e MIDI AdLib, uscita e secondo avvio Windows;
  nella stessa sessione DOS Navigator, Doom e mouse/calcolatrice Costa.
  Tutto superato.
- `scripts/qemu_test_full_hardware_controls.sh`: menu e anteprime con rifiuto
  simulato di cambio modalità e banco video, impostazioni conservate, ritorno
  leggibile al menu; controlli di stato, testo 80×50, luminosità e reset.
  Tutto superato.
- `scripts/qemu_test_full_costa.sh` avviato dalla ISO finale con 512 MB:
  avvio del CD, comando digitato dalla tastiera, desktop Costa, spostamento
  del cursore e apertura della calcolatrice a 640×350. Tutto superato.

Log e schermate: `build/full/t23-global-video-2026-09-06/`.

## Artefatto

- ISO: `build/full/CiukiOS_full_cd_0-7-1.iso`.
- Dimensione: 101261312 byte.
- SHA-256 del file ISO locale:
  `f42309923770e239ea7a792311dee4a618fdf6627fec9b5ec643f2de77de0ae4`.
- La configurazione estratta dalla ISO finale conferma
  `INITRD /ciukios-full-cd-disk.img` e `APPEND harddisk raw`.
- Limite della sessione VS Code verificato: `MemoryMax=8000000000`,
  `MemoryHigh=7000000000` byte.

Masterizzazione completata su `/dev/sr0` con cancellazione rapida del CD-RW,
scrittura DAO ed espulsione richiesta al termine. `xorriso` ha riportato
`Writing to '/dev/sr0' completed successfully.` ed è terminato con codice 0.
Log: `build/full/t23-global-video-2026-09-06/burn.log`.
Non è stata eseguita una rilettura di verifica del disco ottico, come richiesto
dall'utente. Il controllo della configurazione e lo SHA-256 sopra riguardano
esclusivamente il file ISO locale.
