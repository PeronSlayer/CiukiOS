# Shell in inglese e setup grafico — 6 settembre 2026

La shell mostra soltanto `A modern Retro OS` nella schermata iniziale.
Menu applicazioni, guida, informazioni e configurazione dello schermo sono
in inglese. La foto di Ciuki e i driver audio della release precedente sono
conservati.

## Setup

`SETUP` dal live CD apre un wizard VGA 640×480 a 16 colori: desktop verde
petrolio, finestra grigia, barra del titolo blu, font proporzionale, pulsanti
in rilievo e cursore privato. Mouse, frecce, Tab, Invio ed Esc permettono di
scegliere operazione, disco, metodo di formattazione e confermare il riepilogo.
Prima della conferma il setup non scrive sul disco. Tornare indietro annulla
la conferma precedente.

La release crea una **partizione di sistema FAT16 da 96 MiB**, a partire da
LBA 63. La tabella delle partizioni precedente viene sostituita e lo spazio
restante rimane non allocato: non è un editor di partizioni e non espande
ancora il volume a tutta la capacità del disco. Il wizard mostra questa
limitazione prima di iniziare. La formattazione completa riguarda la
partizione di sistema e il relativo spazio iniziale, non tutto il dispositivo.

| Operazione | Effetto verificato |
| --- | --- |
| Rapida | Nuove strutture FAT16, due FAT identiche, directory radice e label; vecchi settori dati conservati. |
| Completa | Azzera i settori della regione di sistema, esegue FLUSH CACHE, poi li rilegge e li confronta con zeri. |
| Installa | Copia e rilegge ogni blocco; modifica il drive di avvio D: in C:; pubblica l'MBR avviabile soltanto alla fine. |
| Annulla / errore | Interrompe la scrittura. Un'operazione incompleta mantiene l'MBR invalidato. |
| Solo formatta | Crea un volume FAT16 vuoto e una partizione inattiva; non dichiara installato un sistema operativo. |

Il setup rileva le destinazioni BIOS 81h–87h, escludendo la sorgente live 80h.
La destinazione scelta viene mappata sul controller ATA e identificata di
nuovo prima della scrittura. Controlla supporto LBA, settori da 512 byte,
capacità sufficiente e layout della sorgente. Non implementa AHCI/NVMe.
È corretto anche un difetto nel vecchio fallback IDE: IDENTIFY veniva inviato
al registro Device/Head invece che al registro Command.

Il formato delle informazioni IDENTIFY e il conteggio LBA seguono la
[specifica ATA/ATAPI](https://read.seas.harvard.edu/cs161/2019/pdf/ata-atapi-8.pdf).
Il font bitmap del setup deriva da Liberation Sans; la
[licenza OFL del progetto](https://github.com/liberationfonts/liberation-fonts/blob/main/LICENSE)
è inclusa come `APPS/SETUPFNT.TXT` e in `src/com/setup_font.LICENSE`.

## Prove eseguite

Risultati in `build/full/setup-refresh-2026-09-06/`:

- `release-full-install.log`: formattazione completa, installazione dalla ISO
  finale, confronto di tutti i byte (unica differenza prevista: D: → C:),
  avvio dal solo HDD e comandi da tastiera.
- `quick-first.log`, `full-fast.log`, `full-tcg.log`: filesystem leggibile con
  mtools, geometria FAT16, due FAT identiche, effetti distinti sui vecchi dati,
  nessuna scrittura oltre la partizione dichiarata.
- `small.log`, `no-disk.log`, `cancel.log`: destinazione piccola o assente,
  navigazione, conferma obbligatoria e annullamento senza variazioni del disco.
- `multiple.log`: due dischi collegati; solo il secondo, selezionato nel wizard,
  viene formattato. Il primo mantiene lo stesso SHA256.
- `interrupt.log`, `io-error.log`: annullamento durante il lavoro e guasto
  di I/O iniettato con QEMU blkdebug; il target resta non avviabile.
- `release-mouse-costa.log`: movimento PS/2 e clic nel wizard, opzioni,
  conferma, annullamento, ritorno alla shell; successivamente mouse Costa
  e avvio della sua calcolatrice.
- `english-shell.log`: shell grafica in inglese, editor della riga, Tab,
  cronologia, F1/F2 e preferenze audio.
- `english-display.log`: menu inglese, anteprima annullabile, timeout,
  risoluzione condivisa, scorrimento, persistenza e ripristino VGA.
- `packaging.log`: catena FAT e byte di SETUP.COM nel CD; nuovo limite
  di packaging di 12 cluster, con limite aggiuntivo per la dimensione COM.
- `payloads.json`: driver audio, launcher Windows/Doom/Wolf e suono di avvio
  identici alla release già collaudata. Il SETUP.COM finale è identico a quello
  verificato nelle prove di formattazione e installazione.

Le prime prove `full.log` e `install.log` sono state interrotte durante
l'ottimizzazione della UI; fanno fede i log finali elencati sopra. La nuova
build non è stata avviata sul T23 fisico durante questa sessione.

## Release

- ISO: `build/full/CiukiOS_full_cd_0-7-1.iso`, 101261312 byte.
- SHA256: `e237dbf4c962bd93d921f15f02fb77f260168701c1cabf31380a00376c2506a9`.
- Boot MEMDISK non compresso, per conservare il percorso compatibile con T23.
- VS Code: limite aggregato della sessione 8000000000 byte.
- Masterizzazione completata su `/dev/sr0`, xorriso exit 0; CD-RW chiuso
  ed espulso. Log: `build/full/setup-refresh-2026-09-06/burn.log`.
  Nessuna rilettura ottica di verifica eseguita.
