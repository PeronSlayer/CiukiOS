# Direttive F1 a Codex: servizi del kernel, FAT/VFS, runner

- **Data:** 2026-10-10
- **Tipo:** decisione (processo) e modifica
- **Versione/commit:** commit di questa voce (contratto F1 in `7befb56`)
- **Stato:** in corso; F0 su hardware ancora da chiudere

## Contesto

Il proprietario ha fissato i ruoli: Claude dà le direttive, rivede, integra
e produce le evidenze su QEMU e hardware; Codex scrive il codice, con modello
ed effort scelti da Claude secondo la complessità. Il contratto di
accettazione F1 scritto da Codex (`docs/design/f1-acceptance.md`) è stato
rivisto dal lead, allineato alla grammatica `safe=1`/`core` già in F0 e
integrato in `main`.

Nel frattempo il proprietario scrive l'immagine F0 (`ciukios-2003adb0.img`)
sul disco Transcend e la prova sul T23 o sull'E500; i risultati avranno una
voce propria.

## Decisioni

1. **Formato delle direttive.** Ogni compito per Codex è un file in
   `docs/directives/` con contratto di riferimento, file toccabili,
   interfacce da non cambiare, test host obbligatori, modello/effort e
   criterio di accettazione; l'indice è `docs/directives/README.md`. Codex
   lavora in un worktree `wt/<task>` e non committa; il lead rivede il diff,
   esegue le evidenze QEMU e integra.
2. **Tre direttive emesse in parallelo**, indipendenti per file e interfacce:
   - `f1-00-kernel-services` (gpt-6-astra, xhigh): mutex dormiente, code di
     attesa, lavoro differito, handle con generazione, ciclo di vita del
     registro con quarantena e catene IRQ PCI condivise, scadenze e `udelay`.
     Nucleo del kernel: il modello più forte.
   - `f1-01-fat-vfs` (gpt-6-astra, xhigh): FAT12/16/32 con nomi lunghi, cache
     write-back con barriere, partizioni MBR/EBR, VFS con handle e share
     mode; tutto compilabile sull'host con iniezione di guasti. I percorsi di
     scrittura e la crash safety sono il rischio maggiore di F1.
   - `f1-02-runner-and-selector` (gpt-6.1-sol, high): grammatica `f1:`,
     azioni del runner (QMP `input-send-event`, `blkdebug`, patch di
     `BOOT.CFG`, riavvio su una sola overlay, export read-only per
     `fsck.fat`/`mtools`), suite F1 e test host. Strumentazione ordinaria.
3. **Interfaccia `blkdev` congelata** dalla direttiva FAT/VFS: il driver ATA
   (direttiva successiva) la implementa, così i due lavori non si bloccano a
   vicenda.
4. **Regola di transizione rispettata:** il codice F1 resta nei worktree
   finché F0 non è chiuso sull'hardware fotografato
   (`docs/design/foundations-transition.md`); l'integrazione in `main`
   avviene dopo, con revisione incrociata.

## Prossimi passi

- Direttive successive: ATA PIO + MBR con ciclo di vita del registro; coda
  di input i8042 nativa; dispositivo framebuffer, presenter e boot log;
  servizio BIOS V86 serializzato (input firmware-first sull'E500).
- Analisi delle foto della prova hardware F0 e voce di diario dedicata.
- Revisione Codex del commit `2003adb` (safe mode e `core`), integrato senza
  revisione per urgenza: da fare nella prima revisione incrociata F1.
