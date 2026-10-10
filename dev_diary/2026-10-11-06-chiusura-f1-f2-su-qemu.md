# Chiusura di F1 e F2 su QEMU: un'immagine i686 per T23, E500 e PC assemblati

- **Data:** 2026-10-11
- **Tipo:** modifica (integrazione) e analisi (regressioni)
- **Versione/commit:** immagine finale indicata in fondo; sequenza di commit su `main` da `aa6009e` in poi
- **Stato:** in corso (batch finale `f2-all` in esecuzione); la qualifica su hardware reale F0→F2 resta da fare dal proprietario su questa immagine

## Contesto

Dopo l'integrazione notturna (voce 2026-10-11-02) restavano consegne sui
rami (`f2-10`, `f1-19`, `f1-20`) e nessuna suite F2 aveva mai girato su
QEMU: i predicati delle suite F2 erano stati scritti dalla tabella del
contratto e provati su record fabbricati. La decisione del proprietario
(voce 2026-10-11-05) richiede un'unica immagine i686 per T23, E500 e PC
assemblati 1998–2004, con i profili QEMU `qemu-desktop-1998` e
`qemu-desktop-2002` accanto a `qemu-t23`, `qemu-e500` e `qemu-min128`.

## Cosa cambia

Fusioni su `main`, in ordine, con direttiva e consegna di Codex per ciascuna
(`docs/directives/README.md` riporta modello, sforzo e stato):

1. **f2-10, f1-19, f1-20** (`aa6009e`, `48f1d12`, `4dc9a0e`): `uname`,
   interruzione delle syscall, checker `fd-table`, `libc-smoke`; IRQ
   trattenute del PIC virtuale sull'E500; baseline hardware e due profili
   desktop.
2. **f2-11** (`4c974c7`): `storage_sync()` restituiva `EBUSY` in guest per
   ogni sonda di scrittura (`fat-write`, `cache`, `bootlog`, `fd-table`)
   perché `files_bootstrap` fissa la cwd del supervisore sulla radice e il
   gancio `px_volume_busy` contava quel riferimento come descrizione aperta.
   Regola: le descrizioni aperte vengono verificate sotto il lock del
   namespace prima di fermare lo storage; i riferimenti cwd e i nodi
   strutturali non bloccano lo smontaggio; un rifiuto lascia lo storage in
   funzione. Test host con il namespace collegato; paragrafi aggiunti a
   `posix-subset.md` e `vfs-storage-contract.md`.
3. **f1-21** (`3db454c`): la sonda `framebuffer` chiedeva 2 247 byte a
   `kmalloc`, oltre la classe massima di 2 KiB dello heap; buffer statici e
   guardia host sulle allocazioni delle sonde.
4. **f2-12** (`9c01ced`) e **f2-16** in tre round (`041ee82`, `a1d6381`,
   `f88126b`): `crash-isolation` con il desktop reale (`server=desktop`),
   chiave `server=` nel selettore del loader (24 064 byte, 47 settori),
   cinque casi desktop e sei stand-in. Cause risolte in sequenza: nessun
   record diagnostico al fallimento dell'handshake (aggiunti `launch` e
   `step`); gli endpoint di `channel_pair` portano `FD_CLOEXEC`, che la
   mappatura fd di `ciuki_spawn` rifiuta (errno 9); i report call-3
   consumati dal controllore venivano anche incorniciati come stream
   applicativi, superando il limite di 64 identità del parser con 100
   vittime.
5. **f2-14** in due consegne (`9a7fcc2`, `a0dc198`): QEMU TCG non genera
   `#AC`; il payload lo riconosce dalla firma CPUID di TCG e usa un page
   fault con AC impostato, mentre su hardware il vettore 17 resta
   obbligatorio (deviazione registrata in
   `docs/validation/2026-10-09-f0/README.md`); tre thread in spin
   ritardavano il risveglio del dormiente oltre la scadenza dei 20 ms, ora
   cedono il processore tra un aggiornamento e l'altro.
6. **f2-13** (`b09a614`): suite `f2-process`, `f2-runtime`, `f2-app`
   allineate ai record reali delle sonde (catture dal guest conservate in
   `tests/host/fixtures/f2/`), con record di evidenza aggiunti a
   `fd-table`, `spawn-wait`, `mmap`, `threads-wait`, `libc-smoke`.
7. **f2-15, f2-17, f2-18, f2-19** (`b09a614`, `0147f99`, `5205433`,
   `62bd77f` e successivi): la sonda `app-gate` non era mai stata
   registrata (`installed=8`, `missing_probe`); implementata (registrata per
   ultima tramite l'elenco sorgenti di `build_kernel.py`, sidecar di
   provenienza, percorso del supplemento riconciliato); doppio fault per
   overflow dello stack del kernel nella costruzione dei record di metadati
   (frame da 2 180/2 348/1 280 a 668/276/256 byte, guardia
   `-Wframe-larger-than` 4 KiB nel kernel e 1 KiB per sonda e supervisore,
   regressione host su stack guardato da 8 KiB); `files.lua` falliva su
   `os.getenv"PATH"` (aggiunto `PATH=/bin` all'ambiente del gate; audit di
   `files.lua` su un harness nativo di riferimento, nessun'altra lacuna);
   verdetto finale con attribuzione della crescita di pagine al pool dello
   heap tramite un registro di sola lettura (`kheap_snapshot`) e dei byte
   alle identità trattenute del namespace.
8. **f1-22, f1-23, f1-24** (`3d969fb`, `deb43ca`, `6084e21`): sei fixture
   FAT32 corrotte deterministiche (BPB non valido, bit dirty, bit errore
   hardware, divergenza delle copie FAT, catena ciclica, settore di
   directory troncato) secondo la specifica Microsoft FAT; taglio di crash
   con gate blkdebug armato solo dopo il record `ARM` all'indice dichiarato;
   esito interrotto dichiarato per la voce LFN orfana del file di lavoro
   (`fat_create` pubblica la voce a nome lungo prima della voce breve);
   record `firmware_overrun` e `disallowed_io` dal self-test della VM BIOS,
   con rifiuto `not_run` fuori da QEMU.

Colla del lead, registrata nei commit: fixture FAT16 per i casi
`fd-table`; correzione degli indici di `fault-repair` e dell'indirizzo di
fault nullo nei predicati; fixture di allineamento e test host dell'SDK
aggiornati alla cattura passante; aggiunta fixture e stato nella tabella
delle direttive.

## Verifiche

Evidenze per immagine in
`docs/validation/2026-10-11-integrated-image-f0-regression.md`
(immagini dalla decima alla diciottesima). Sintesi sull'immagine
`9ae41b27…` e precedenti dello stesso stato:

- F0: `f0-core` 92/92 sui cinque profili.
- F1: `f1-input` completa (registry, input nativo e firmware su T23/E500,
  `framebuffer` e `framebuffer-no-lfb`, `firmware_overrun`, `disallowed_io`,
  casi desktop-1998/2002); `f1-safe` 106 casi QEMU; `f1-storage` e
  `f1-fat32` complete, compresi `cache*`, `fat-write`, `bootlog*`, i sei
  `mount-*` corrotti e `mount-crash-reboot` su due avvii.
- F2: `f2-process` e `f2-runtime` (sette casi) passano tramite runner su
  `qemu-t23`, `qemu-e500`, `qemu-min128`; `crash-isolation` con il desktop
  reale passa sui cinque profili (100 cicli, interazione post-fault
  consumata, osservazione dello schermo) e i sei casi stand-in sui tre
  profili; `app-gate`: la suite ufficiale di Lua 5.4.8 con `_U=true` esce
  zero in guest (72,9 s) e il supplemento passa (127,8 s).
- Test host: 170 OK sull'albero `6084e21`.

Il batch finale `f2-all` (tutte le suite in ordine, cinque profili)
sull'immagine finale e il suo esito sono riportati nell'aggiornamento di
questa voce.

## Decisioni

- Nessuna immagine per singola macchina: i profili QEMU sono configurazioni
  di prova; il loader riconosce la piattaforma all'avvio.
- Il self-test della VM BIOS resta limitato a QEMU; su hardware le due
  sottocase valgono `not_run` (livelli T0/T3 del contratto).
- La voce LFN orfana dopo un taglio di crash è un esito interrotto
  dichiarato; nessuna modifica al driver.
- Le sonde non usano `kmalloc` per fixture oltre la classe massima dello
  heap; il kernel compila con `-Wframe-larger-than`.

## Prossimi passi

Qualifica su hardware reale F0→F2 (T23, Armada E500, PC assemblati) con
l'immagine finale scritta sul disco Transcend tramite
`scripts/test/write_physical.sh`, cattura seriale a 38400 8N1 con
`scripts/test/serial_capture.sh` e import con `run.py --physical-capture`.
