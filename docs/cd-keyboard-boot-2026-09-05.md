# CD: tastiera bloccata e caricamento in RAM

Aggiornamento del 6 settembre: il T23 si blocca durante la decompressione
MEMDISK della variante gzip descritta qui. La release usa nuovamente il disco
RAM non compresso; le correzioni PS/2 restano incluse. Vedere il
[riscontro e la nuova build](global-display-boot-2026-09-06.md).

## Riscontro

Dopo la ISO `a74e1c8f…8a07e`, l'utente riferisce una lunga attesa durante il
caricamento El Torito/MEMDISK e nessun carattere digitabile dopo l'avvio di
CiukiOS. La macchina ha almeno 512 MB di RAM. La precedente indicazione di
attesa dopo la schermata CiukiOS è stata corretta dall'utente.

## Tastiera e mouse

L'ultima distribuzione aveva attivato l'inizializzazione PS/2 anche sul CD
per ripristinare Costa. Quel percorso leggeva il byte di configurazione
8042 dopo aver abilitato AUX, senza fermare i dispositivi. Tastiera, mouse
e risposta del controller condividono la porta 60h: un dato in arrivo poteva
essere scambiato per la configurazione e riscritto disabilitando IRQ1, clock
della tastiera o traduzione dei codici BIOS.

Il test esegue il codice macchina del vecchio kernel contro un controller
con dati intercalati. Riproduce IRQ1 disabilitato sia con un tasto sia con un
pacchetto TrackPoint arrivato nella finestra critica. Riproduce inoltre byte
di tastiera consumati da IRQ12 e dall'attesa dell'ACK mouse.

Correzioni:

- Inibire entrambi i dispositivi prima di svuotare il buffer e leggere la
  configurazione; conservare traduzione e bit di sistema del BIOS.
- Riabilitare esplicitamente clock e IRQ1 della tastiera anche se il mouse
  rifiuta il comando o non risponde. Conservare lo stato IF del chiamante.
- Attendere gli ACK AUX senza leggere un byte proveniente dalla tastiera;
  accettare l'ACK dopo eventuali pacchetti precedenti, con limite di attesa.
- Nell'handler IRQ12 controllare il bit AUX prima di leggere la porta 60h.
  Un interrupt rimasto pendente non deve sottrarre un tasto al BIOS.

Il protocollo di separazione delle porte è descritto nella
[documentazione del controller Intel](https://intel.github.io/ecfw-zephyr/reference/kbchost/index.html).
Il test è `scripts/test_ps2_boot.py`, eseguito sul kernel realmente assemblato
tramite Unicorn. Dodici casi verificano anche traduzione BIOS disabilitata,
IF inizialmente zero, mouse assente, rifiuto F6/F4, timeout del controller e
ricezione di un vero byte mouse. Il kernel finale occupa 42960 byte su 43264.

## Caricamento del CD

La partizione e tutti i programmi restano gli stessi. Il disco MEMDISK viene
compresso con gzip, supportato dal
[loader Syslinux](https://kernel.googlesource.com/pub/scm/boot/syslinux/syslinux/+/syslinux-6.03-pre14/doc/memdisk.txt).
I dati da leggere prima dell'avvio scendono da 100695552 a 46366896 byte
(circa 54% in meno). Questo misura i byte letti, non il tempo effettivo del
lettore sul T23, che va confermato sull'hardware.

La decompressione richiede temporaneamente entrambe le copie. Il builder
calcola un budget di 149 MiB includendo 8 MiB di margine, compatibile con i
512 MB indicati. Per le macchine da 128 MiB conserva l'immagine separata
`build/full/ciukios-full-cd-lowmem.iso`, senza compressione. Il launcher QEMU
la seleziona automaticamente sotto 160 MiB. Si può anche costruire la release
non compressa con `CIUKIOS_FULL_CD_COMPRESS=0`.

## Verifiche e artefatti

Cartella: `build/full/t23-cd-keyboard-2026-09-05/`.

- `ps2-before.log`: cinque difetti riprodotti sul kernel precedente.
- `ps2-after.log`: dodici casi PASS sul kernel corretto.
- Build HDD e CD completate; controlli sintattici e `git diff --check` PASS.
- `windows-1024.log`: Windows 1024x768, puntatore singolo, WAV/MIDI, uscita,
  DOS Navigator, verifica stato DOS, secondo Windows, Doom originale e
  Costa con cursore e tastiera funzionanti nello stesso avvio: PASS.
- `costa-compressed.log`: avvio della vera ISO compressa in QEMU a 512 MiB,
  lancio dalla shell, desktop Costa, cursore (52 pixel modificati) e
  Calculator aperto da tastiera: PASS.
- `setup-compressed.log`: installazione dalla ISO compressa con 512 MiB e
  CD virtuale espulso prima di SETUP, confronto completo del payload clonato
  e avvio autonomo dell'HDD installato con 128 MiB: PASS. Il confronto è
  limitato agli artefatti QEMU, non è una rilettura del supporto ottico reale.
- Launcher QEMU: selezione ISO primaria con 512 MiB e lowmem con 128 MiB
  verificata tramite `--dry-run`.
- `costa-lowmem.log`: ISO non compressa a 128 MiB, tastiera della shell,
  desktop Costa, movimento del cursore e Calculator: PASS.
- `burn.log`: CD-RW `/dev/sr0` masterizzato correttamente ed espulso,
  processo terminato con codice 0. Nessun controllo preliminare separato
  del disco e nessuna rilettura di verifica del supporto, come richiesto.

Comando di masterizzazione:

```sh
xorriso -as cdrecord -v dev=/dev/sr0 blank=fast -dao -eject build/full/CiukiOS_full_cd_0-7-1.iso
```

ISO primaria: `build/full/CiukiOS_full_cd_0-7-1.iso`, 46934016 byte.
SHA-256: `ab1d175ab363ad1aa1d5204d18a3f34e7ff1ad118d64aba665413d629fab24df`.

Il limite della sessione VS Code è stato riapplicato allo scope corrente
`app-code-111849.scope`: massimo 8000000000 byte, soglia 7000000000 byte.
Le modifiche già presenti nel workspace sono state conservate.
