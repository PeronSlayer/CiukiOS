# Integrazione notturna: shell del desktop, nucleo VM DOS, audio AC'97, ATAPI/ISO 9660, NTFS, loader; qualifica QEMU e difetti corretti

- **Data:** 2026-10-11
- **Tipo:** modifica (integrazione) e analisi
- **Versione/commit:** da `266bbf6` a `2af7645` (fusioni) e commit successivo delle correzioni; immagini `a71327b7…` (batch completo) e `38a7b4e7…` (correzioni)
- **Stato:** in corso (campagna `vm-boot` e batch finale da eseguire)

## Contesto

Dopo la decisione di accelerare (voce 2026-10-11-09) sono state consegnate
e fuse su `main`: f2-30 (shell del desktop: barra, menu Ciuki da `/bin`,
riduzione a icona, richiesta di chiusura con protocollo v2, finestra About,
icone Tango), f3-00 (nucleo delle VM DOS: ciclo di vita e ammissione, disco
privato FAT16 `DOSBOOT.IMG` con overlay su file, BIOS virtuale, avvio di
CIUKIDOS con la shell COMMAND_COMPAT, canale record sulla porta E9, sonda
`f3:vm-boot`), f1-37 (loader: dopo S la finestra del menu riparte e accetta
P), f4-01 (`/dev/audio`, `/dev/mixer`, syscall 69 `devctl`, mixer intero,
backend Intel ICH AC'97, persistenza del volume), f4-02 (ATAPI PIO, interfaccia
`fs_ro`, ISO 9660/Joliet, fixture xorriso), f4-03 (libreria `ntfs_ro` con 27
fixture). Il contratto F4 è stato approvato (voce 2026-10-11-10). Le
riconciliazioni tra consegne parallele (tabelle delle sonde F3/F4, menu del
loader, linker, importer, harness) sono registrate nei commit di fusione.

## Cosa cambia

Le consegne non erano verificabili su QEMU da Codex; la qualifica su QEMU ha
rivelato difetti reali, corretti dal lead:

1. Stack dei task kernel a 32 KiB: il percorso della sonda ottica
   (`probe_optical` → `listing` → `readdir` → `read_file`, frame da circa
   3 KiB l'uno) andava in double fault a 16 KiB.
2. Scoperta ATA: un dispositivo PACKET dopo il reset tiene DRDY a zero; la
   firma `14h/EBh` lo identifica, non lo stato 0 (prima scartato come assente).
3. Audio: `proc_spawn_file` restituisce il PID e la sonda lo trattava come
   errore; il task di refill non veniva avviato (`task_start` mancante); coda
   di silenzio di 500 ms prima dello stop del trasporto, riempita in un colpo
   solo con 28 periodi all'inizio della coda perché, mentre i client escono,
   il worker non viene eseguito per circa 130 ms (`max_service_gap_us`); le
   sottoalimentazioni in coda non contano come `halt`; il tempo di controllo
   (stop, burst) è contabilizzato a parte dal budget di 1 ms del refill.
4. Runner: QEMU 11.1.1 non ha più `change`; il supporto ottico si cambia con
   `eject` e `blockdev-change-medium` indirizzando il drive per nome; il
   checker WAV accetta l'intestazione RIFF non finalizzata da QEMU e la
   registra.
5. Caso `desktop-shell`: il menu Ciuki è alto `28 × (programmi + 2)` px sopra
   la barra e l'immagine canonica ha quattro programmi in `/bin`; il modello
   PS/2 di QEMU consegna al massimo cinque pacchetti da ±127 px per evento,
   quindi gli spostamenti a 2048 px dello script erano imprevedibili; i tasti
   A si contano dalla richiesta di chiusura (stadio 5) perché il reap è
   asincrono.

## Verifiche

- Immagine `a71327b7…` (correzioni del terzo sweep): batch `f2-all` completo,
  168 casi distinti su 171 superati; i tre `uart-absent-*` richiedono
  conferma visiva. Immagine hardware `ad63279d…` (run `88888888`) consegnata
  al proprietario per il quarto sweep.
- Immagine `38a7b4e7…` (fusioni e correzioni): `desktop-shell-qemu-t23`,
  `desktop-shell-qemu-e500`, `optical-l1-qemu-t23`, `audio-native-qemu-t23`
  superati; il WAV catturato dura 4,0 s con tutte e sei le finestre a
  440/880 Hz e RMS nei limiti. Restano da eseguire la campagna `vm-boot` sui
  tre profili, `f0-smoke` e il batch `f2-all`.

## Decisioni

- Codex ha esaurito il credito d'uso (riprova il 16 ottobre o con nuovi
  crediti): le quattro direttive della seconda ondata (f3-01, f3-02, f3-03,
  f2-31) sono state interrotte a metà; per la regola del proprietario il lead
  le completa direttamente, a partire da f3-03 (sonda `vm-memory` scritta).
- Lo stack kernel di 16 KiB fissato da f2-20 passa a 32 KiB per tutti i
  task; il taglio dei frame del percorso ottico resta un seguito.
- La coda di silenzio prima dello stop del trasporto e il cuscinetto di
  28 periodi in coda sono politica del backend AC'97 (contratto F4,
  "Stop an idle backend only when…"), registrata in `f4-acceptance.md` al
  prossimo aggiornamento.
