# Quarta prova sul T23 (immagine e86305de): audio nativo attivo, UDMA, mouse USB; dieci direttive

- Data: 2026-10-13
- Tipo: analisi hardware, decisioni
- Immagine: `build/hw/ciukios-hw.img` SHA-256 `e86305de…` (main `707e23cf`), avvio `L`, cattura `legacy/local/physical/e86305de-usb-storage-fault-optical-missing/`

## Fatti misurati

- **Audio nativo**: tutte le fasi `[audio] stage=…` con `result=ok` (il registro 26h arriva a 000Fh dopo il reset del codec di f4-10); il suono di avvio si sente sul T23.
- **Disco**: `[ata0] dma mode=udma2 … cable80=0`; Terminal (2,5 MB) caricato in 236 ms (`dma_ms=80`), DOS Box in 31 ms. Prima: 1,6–1,9 s per 1,3–1,5 MB.
- **USB**: tre controller UHCI dell'ICH3 (00:1d.0/1/2) inizializzati; ricevitore Logitech 046d:c52b enumerato (tastiera e mouse HID, 188 ms per il primo descrittore); il mouse funziona. Chiavetta Lexar 05dc:a81d: enumerata, Get Max LUN=0, reset BOT ok, poi `[usb-storage] fault addr=2 reason=data_transfer tag=1` e `attach result=-5`.
- **Ottico**: `[ata1] unit=0 kind=atapi` identificato, ma `[storage] optical slot=2 drive=- result=-5 reason=unavailable` anche con un disco inserito.
- **Savage**: il percorso a porte supera il test del lock e imposta CR40.0, ma `quarantined reason=mmio_alias_dead`; il marcatore del tentativo costa 338 ms per avvio (`phase=marker_write`).
- **Desktop**: aggiornamenti piccoli (9 kpx) in 354 µs di presentazione e 427 µs di composizione; il proprietario percepisce ancora scatti su spostamenti di finestre e menu.
- **DOS**: DOOM parte e gira ma lento, Wolfenstein 3D meno lento; nessun audio dalle VM DOS mentre l'audio nativo funziona; il testo di avvio di CiukiDOS è visibile prima del programma.
- **f1-usb su QEMU**: dopo f1-45 (task USB mai avviato) la tastiera USB viene enumerata in 10 ms; il caso resta fallito per `processes_equal=0` nella sonda: il thread dello sfondo nasceva dopo il primo fotogramma, dopo la baseline della sonda. Correzione del lead in 94367ea2 (thread creato all'avvio, richieste solo dopo il primo fotogramma).

## Decisioni

- Dieci direttive Codex (c008ef1c): f1-47 fase dati USB storage, f1-48 lettore ottico, f1-49 Savage solo porte, f2-50 architettura di rendering, f2-51 AutoPlay, f2-52 demo grafica, f2-53 barra, f3-13 DOS Box (finestra di caricamento, prestazioni), f4-12 suoni di sistema, f4-13 audio delle VM DOS.
- Il pacchetto driver resta sorgente di ID e di contratto (f6-03); nessun host NDIS senza decisione del proprietario.
