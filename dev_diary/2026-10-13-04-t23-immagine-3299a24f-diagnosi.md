# Terza prova sul T23 (immagine 3299a24f): diagnosi di accelerazione, disco, presentazione, DOOM e rete

- Data: 2026-10-13
- Tipo: analisi hardware, decisioni
- Immagine: `build/hw/ciukios-hw.img` SHA-256 `3299a24f…` (QEMU `9451e072`, main `f9f112a0`), avvio `L`, cattura seriale `legacy/local/physical/3299a24f-savage-crtc-locked/`

## Fatti misurati

- La correzione dell'ordine dei mutex (f9f112a0) regge: avvio completo fino al desktop, lancio di CiukIDE, Files, DOS Box, Pannello di controllo, spegnimento ACPI regolare.
- **Savage 2D**: il tentativo marcato (`attempt persisted`) esegue `crw 38`, `crw 39`, `crr 38` e si ferma in `quarantined reason=crtc_locked`: la rilettura di CR38 attraverso l'alias MMIO (BAR0+0x83d4/5) non restituisce 48h. Il driver usa solo l'alias MMIO; il driver X.org `savage` abilita l'accesso MMIO con le porte I/O VGA (0x3c3, 0x3c2, CR40.0 = 1 via 0x3d4/5) prima di qualunque accesso MMIO. Decisione: direttiva f1-38.
- **Presentazione**: 1024x768x32, MTRR WC su tutto il LFB (`size=4194304 ok`); un fotogramma intero costa 59 ms (`present_us=118000` per 2 fotogrammi, 53 MB/s) perché `fbdev_present_to` converte pixel per pixel. Decisione: f1-40 (copie per riga, `rep movsd`).
- **Composizione**: 130 ms per fotogramma intero (`composite_us=265000` per 2 fotogrammi; 6–9 Mpx/s). Decisione: f2-45.
- **Disco**: `[launch-io] … read_max=4096 disk_requests=847 disk_max=512`: una richiesta ATA per settore; ELF da 1,3–1,5 MB in 1,6–1,9 s (circa 800 KB/s). `storage_work` supera il budget a ogni esecuzione. Decisione: f1-39 (richieste fino a 64 KiB, READ MULTIPLE).
- **Avvio del desktop**: prima finestra di statistiche con `input_lag_us=11047000`; lo sfondo (`Ciuk2.png`, 2,56 MB, 1448x1086) viene letto, decodificato e riscalato in modo sincrono. Decisione: f2-44 (record di fase, decodifica non bloccante, cache dei fotogrammi resi, pre-rendering in fase di build).
- **DOOM**: la VM entra in modo protetto, 721 tick IRQ0 consegnati, poi dopo `vm_if restore=1` a 01df:247fac il PIC virtuale resta con IRQ0 in servizio (`isr=01`, `irr=01`), `delivered` fermo a 721 mentre gli `edges` crescono; il client gira a 01df:2483dc/2483e2. Inoltre 41 record `vm_db` con `tf=1 armed=0` dal primo istruzione della VM. Decisione: f3-11 (secondo tentativo con Sol; poi Astra).
- **Rete**: `[net] driver=e100 … link=down` all'avvio e `link=up` dopo l'inserimento del cavo; nessuna interfaccia utente. Decisione: f6-02 (`/dev/net`, icona nella barra).
- **Audio**: `error=-110 reason=stop_unproved` come prima; f4-09 (Codex) fuso in main (20011bf9): ordine di reset, ripiego a 48 kHz, prova DMA silenziosa, record `[audio] failure stage=…` per la prossima cattura.

## Decisioni

- Sette direttive in parallelo a Codex (`gpt-6.1-sol`, coda a tre sessioni): f1-38, f1-39, f1-40, f2-44, f2-45, f6-02, f3-11; il lead fonde, ricostruisce un'unica immagine e la prova sul T23 con cattura seriale.
- Nessuna campagna QEMU lunga: prove su hardware reale (decisione del proprietario del 12 ottobre).
