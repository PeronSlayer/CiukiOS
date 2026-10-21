# 2026-10-21-10 — Prova T23 dell'immagine 1.0.0 alpha (8f59e221…): esito negativo

## Esito (proprietario, 23:40–23:58)

NetSurf non si apre; le finestre continuano a scattare; qualsiasi
applicazione o gioco DOS manda il sistema in protezione con errore di I/O;
la copia di un file da chiavetta USB è lentissima; Proprietà schermo va
subito in «non risponde»; la finestra del supporto rimovibile ha lo sfondo
trasparente e illeggibile; lo sfondo del desktop si carica dopo l'ingresso
al desktop; NetSurf è senza icona e sotto «Development». L'immagine non è
presentabile; i log del disco non risultano copiati (la directory
`legacy/local/physical/8f59e221/` non esiste); l'evidenza è la cattura
seriale (`build/lead-scratch/t23-alpha-8f59e221-serial.txt`, 6.974 righe).

## Cause individuate dalla seriale

1. **ATA, quarantena DMA su interrupt anticipato** (`ata.c:1066-1082`):
   `dma quarantine … command=25 bm=24 status=50 error=00 elapsed_ms=1-2
   fallback=pio` all'avvio e al primo gioco DOS; il DMA non viene avviato
   perché un fronte di interrupt arriva fra `issue()` e lo start, poi il
   comando completato viene classificato come fallito. Segue il fault PIO
   `command=29 status=58` (READ MULTIPLE EXT con DRQ) → controller fermato →
   `readonly reason=device_quarantined file=UNIFIED.IMG`, `failure
   stage=writeback result=-5 fallback=readonly`: errori di I/O nei giochi
   DOS, copia USB in PIO. Stessa firma presente una volta nella prova
   b51f034f. Direttiva **f1-92**.
2. **Prenotazioni di volume native su letture a freddo**: `vfs_hold drive=C
   ms=2294 task=desktop op=lookup`, `ms=1601/1588/1338/1291 op=stat`
   (waiters 1–4), `ms=1910 task=dosvm op=mkdir`; comandi dispositivo fino a
   `svc_max_ms=106`. Il modello T23 di f1-89 misurava 0 ms perché a cache
   calda. Direttiva **f1-93**.
3. **NetSurf dal menu senza canale desktop**: `[console] Unable to initialise
   framebuffer` (`nsfb_surface.c:34`, `ciuki_browser_channel<0`); solo il
   percorso di test GATE_BROWSER passa `--channel-fd`. **Entropia**:
   `[entropy] sources=0 irq_mask=0000 health=fail` dopo 2.644 round in
   335 ms: jitterentropy non si inizializza sul Pentium III, HTTPS non
   sarebbe mai pronto. Voce di menu e icona mancanti. Direttiva **f6-19**.
4. **Proprietà schermo**: nessun record di uscita del task `display-#39`;
   causa da riprodurre sull'host (chiamate host dell'applet, euristica
   «non risponde» del desktop). Direttiva **f2-88**.
5. **Finestra supporto rimovibile trasparente** → **f2-89**; **sfondo del
   desktop caricato dopo il primo frame** (`wallpaper_wait=mailbox-wait`) →
   **f2-90**; richiesta del proprietario di una barra di caricamento
   moderna sullo splash con l'immagine di Ciuki → **f0-55**.

## Decisioni

- L'immagine 8f59e221… è respinta; la prossima immagine della 1.0.0 alpha
  richiede la fusione di f1-92, f1-93, f6-19, f2-88, f2-89, f2-90, f0-55,
  le suite host verdi, la build canonica e, prima della consegna, il boot
  smoke QEMU con screenshot dello splash (concesso dal proprietario per
  l'immagine hardware).
- Il kernel DOS unificato resta predefinito: gli errori di I/O derivano
  dallo storage in sola lettura (punto 1), non dal kernel.
- Sette agenti Claude lanciati alle 23:58 nei worktree omonimi; il
  proprietario riprende al mattino.
