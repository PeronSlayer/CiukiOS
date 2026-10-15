# Terza prova del T23 (immagine 5e018550), cambio di metodo e immagine a395ffa8: volume che si ripara, scatola nera, pagina di autodiagnosi, mouse USB, seriale

- **Data:** 2026-10-15
- **Tipo:** analisi (hardware reale), decisione (metodo), modifica (filesystem, kernel, driver, loader), immagine
- **Versione/commit:** 0.9.0 pre-alpha / main `f575e53a`; immagine QEMU `945c21d601e8a4ca…`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `a395ffa896668a6e51ca43d9fdbe2e0f0ebada3cf2cbb19aa89f55c6e4a69b07`
- **Stato:** completato

## Contesto

Prova del proprietario sull'immagine `5e018550` (voce 2026-10-15-05): primo avvio inutilizzabile, spegnimento forzato, poi "I/O error" su ogni operazione (terza volta); mouse USB a scatti, senza tasto destro né rotellina. Evidenze in `legacy/local/physical/5e018550/`: seriale ancora interallacciata; log su disco del solo secondo avvio (nessuna rotazione, nessun record del motivo della sola lettura: il log muore con il volume); descrittore HID completo del ricevitore Logitech (report ID 2, 16 pulsanti, X/Y a 12 bit, rotellina, scorrimento orizzontale); riempimento Savage ancora `b6b6b6b6` con ogni registro riletto uguale a quanto scritto (`MM8136` e `CR45` non trattengono il valore). Il proprietario ha chiesto una soluzione definitiva invece di correzioni caso per caso.

## Decisioni

- Nessuna immagine senza una pagina di autodiagnosi hardware: ogni correzione hardware si verifica sulla pagina (foto), non con la prova di gioco.
- Il volume di sistema non deve mai bloccarsi per i propri file e deve ripararsi dopo uno spegnimento sporco come fanno gli altri driver FAT; le evidenze di un blocco devono sopravvivere in settori riservati fuori dal filesystem.
- La seriale trasmette record interi o nulla, con quota alla capacità della linea.

## Cosa cambia

- **f1-68, mouse USB.** Decodifica dal descrittore esatto (fixture dai 148 byte del log): 16 bit di pulsanti, assi a 12 bit con segno, rotellina e scorrimento; report malformati conservano i pulsanti; record `[desktop] usb wheel` dal consumatore comune (caso QEMU); la coda del client accorpa solo le posizioni assolute; il toolkit scorre liste, alberi, aree di testo e barre con la rotellina. 262 ns per report su host, nessuna attesa nel percorso.
- **f0-24, seriale.** La sezione da 2,1 ms a interruzioni spente era lo scrittore UART (lettura di prontezza e fino a sedici scritture con IF spento); i burst del log superavano di 5–10 volte la capacità di 38400 8N1. Ora un solo consumatore (task del log), cursori persistenti per record, riempimento della FIFO che non attraversa mai un record, un lock breve per byte, quota al 75 % della linea per 1000 tick con scarto di pubblicazioni intere contate, nessun credito accumulato a riposo, panic che completa il record in corso.
- **f0-25, volume e scatola nera.** La scansione di proprietà revocava le scritture per qualsiasi cluster perso (anche un'allocazione COW non pubblicata) e il confronto fra le copie della FAT trattava come conflitto un'interruzione fra gli aggiornamenti dei flag; la rinomina nella stessa directory di una voce 8.3 importata pretendeva spazio LFN e poteva fallire con ENOSPC, impedendo la rotazione di `BOOT.LOG`; un fallimento di scrittura in espulsione dalla cache faceva fallire la lettura che l'aveva incontrato; una base dei giochi leggibile non si apriva se il file COW non poteva essere creato su un volume in sola lettura. Ora: montaggio iniziale in sola lettura con scansione limitata; il passaggio in scrittura ritaglia le tre catene dei log alla lunghezza pubblicata, concilia allocazioni e flag non posseduti, libera i cluster persi e riavvia la sessione sporca con ordine durevole; restano bloccanti solo cross-link vivi, disaccordo vivo fra le FAT, metadati non validi ed errori I/O emessi; la rinomina esatta riusa la voce; le vittime sporche fallite restano appuntate; EROFS ammette un disco DOS in sola lettura. Scatola nera: descrittore CRC32 al settore 8 della partizione e anello grezzo da 64 KiB nei settori 32–159 (riservati da `scripts/build_image.py`), scritto direttamente sul dispositivo a blocchi fuori da FAT, cache e gate, sequenza/lunghezza/CRC32 per settore, riprodotto sulla seriale prima del montaggio e come testa immutabile `[blackbox]` del nuovo `BOOT.LOG`; scrittura PIO d'emergenza di un settore nel panic. Verifica host: 964 tagli di corrente sul log e sulle impostazioni, 2122 sul filesystem, 512 prefissi di settore lacerato.
- **f0-26, pagina di autodiagnosi.** Voce "H" del menu di avvio: nove controlli ordinati con esito a schermo, nel log e nella scatola nera (scrittura e rilettura ATA, taglio di corrente FAT32 con riparazione, chiavetta con tempo, layout HID e cinque secondi di eventi del mouse decodificati, tasti PS/2 ed EC, timer con massimo a interruzioni spente attribuito, 100 record seriali con biglietto, Savage, codec con tono); ogni voce entro 15 s; dispositivi assenti saltati; ripristino obbligatorio. Prova a tappeto Savage: 96 configurazioni per profondità (bit 3 del descrittore, BW_DISABLE, passo in pixel o qword, CR50 00/01/11, formati lineare/destinazione/16/32) con riempimenti in una regione VRAM fuori schermo salvata, poi il modo 1024×768×16 tramite il servizio display; stampa la prima configurazione esatta o `none`; la produzione legge `EngineConfig` (0..95) dalla chiave di registro `System\Display\Savage`.
- **f0-27.** La quota di linea di f0-24 si applicava anche su QEMU e scartava sedici record di avvio (fra cui `[supervisor] bootstrap` e `desktop pid=`) che il runner esige: le pubblicazioni protette (prova e avvio) attendono ora la quota in contesto di task; la firma TCG o un selettore di prova valido porta la quota alla velocità del chardev; l'hardware conserva il 75 %.
- Riconciliazioni nel checkout principale: f0-26 con f0-24 (`serial.c`, `kernel.h`, test), f0-25 con f0-24 e f0-26 (`output.c`, `fat.c`, test; gancio della scatola nera collegato).

## Verifica

- Suite host: kernel (inclusi filesystem, USB, seriale, scatola nera), desktop 101/101, gate 10/10, payload 9/9, immagine DOS, API di riferimento.
- Catena finale: sdk, desktop 101/101, gate 10/10, apps, build-full con albero pulito, suite host del kernel, payload 9/9, immagine T23, f0-smoke 4/4 (l'immagine intermedia `c074bb8f` aveva fallito l'avvio normale per la quota seriale: f0-27).
- Gate QEMU: f1-usb 3/3 (anche il caso della rotellina), desktop-shell PASS, f5-core 2/2, f1-input 24/24, audio nativo 2/2, vm-window PASS, dos4gw PASS; vm-audio FAIL (scadenze su TCG, noto); vm-fullscreen FAIL (rilettura VGA sotto QEMU, f3-29); quake: parte e renderizza (320×200, 32 cambi di schermo in 233 s) ma la sonda scade a -110 per la lentezza del rendering software sotto TCG (f3-30).
- Sul T23: avvio con "H" e foto della pagina; spegnimento forzato e riavvio senza errori I/O con `BOOT1.LOG` e testa `[blackbox]`; mouse con tasto destro e rotellina; seriale leggibile.

## Riferimenti

- `docs/directives/f1-68…f0-26`, `docs/validation/f0-24-serial-hardware.md`, `docs/validation/f0-25-volume-recovery-and-blackbox.md`, `docs/design/hardware-check.md`, `docs/design/savage-2d.md`.
