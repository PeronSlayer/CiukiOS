# Accelerazione: F3 e la shell del desktop partono su QEMU in parallelo alla coda hardware di F0–F2

- **Data:** 2026-10-11
- **Tipo:** decisione del proprietario e pianificazione
- **Versione/commit:** `main` a `266bbf6`
- **Stato:** in corso

## Contesto

Terzo sweep hardware sul ThinkPad T23 (cattura `77777777`, immagine
`6ed146ab…`): 29 passi superati su 35, 2 `not_run` attesi; i quattro
fallimenti residui sono criteri di prova scritti per QEMU (totali esatti
dello stimolo `input`, uscita del desktop sotto un flusso di eventi umano,
passo `safe` eseguito fuori dalla modalità sicura) e un predicato della
sonda `channel-eintr` che su hardware campiona `T_READY` invece di
`T_BLOCKED`; direttive f1-34, f1-35, f1-36 e f2-23 in lavorazione. La
linea 0.8 offriva desktop con applicazioni, finestre DOS V86 con VGA,
input e audio virtuali, rete e browser.

## Decisione

Il proprietario chiede passi più grandi per riportare sulle nuove
fondamenta le funzionalità della linea 0.8. Si decide:

1. **F3 (VM DOS) inizia subito su QEMU**, con direttive parallele tagliate
   dal contratto già approvato `dos-dpmi-contract.md`: f3-00 nucleo della
   VM (ciclo di vita, disco privato FAT16, BIOS virtuale, avvio di
   CIUKIDOS fino al prompt), poi f3-01 PIC/PIT/tastiera/mouse per VM,
   f3-02 VGA virtuale e finestra sul desktop, f3-03 A20/XMS/UMB/EMS,
   f3-04 DPMI 0.9 nel kernel, f3-05 stato SB16/OPL, f3-06 carichi di
   lavoro e payload di prova. I modelli di `src/vm/` si riusano secondo
   l'inventario di `foundations-transition.md`.
2. **Traccia desktop in parallelo:** f2-30 shell del desktop (cornici,
   barra, menu Ciuki, avvio dei programmi, About), poi le applicazioni
   (Files, CiukNote, Tasks, Pannello di controllo) come programmi nativi.
3. **F4** richiede il contratto prima del codice: la bozza di
   `f4-audio-media-contract.md` è affidata a Codex (direttiva f4-00),
   seguita dalla revisione del lead e dalle direttive.
4. **La qualifica hardware di F0–F2 continua** con gli sweep del
   proprietario sull'immagine integrata corrente; i gate F0–F2 su QEMU
   restano obbligatori su ogni immagine e le sonde F3 entrano nello sweep
   appena esistono.

## Motivazione tecnica

Il contratto F3 è approvato e revisionato in modo incrociato dal 9
ottobre; il kernel ha già un monitor V86 con PIC e PIT virtuali usato per
il firmware (`src/kernel/vm/v86.c`, `biosvm.c`); i modelli VGA, periferiche,
audio e OPL della linea 0.8 (circa 4 600 righe con test host) sono
dichiarati riusabili. I fallimenti residui su hardware riguardano criteri
di prova, non funzioni del sistema, e non giustificano di tenere ferma la
fase successiva.

## Verifiche

Nessuna nuova verifica in questa voce; le direttive sono in
`docs/directives/` e i risultati andranno nelle voci successive.
