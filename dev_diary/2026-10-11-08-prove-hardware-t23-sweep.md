# Prove su hardware: ThinkPad T23, sweep automatico e difetti reali corretti

- **Data:** 2026-10-11
- **Tipo:** analisi hardware e modifica (infrastruttura di prova, driver, kernel)
- **Versione/commit:** da `6da9b9c` a `b89927c`; immagini provate sul T23 `b9c052cd…`, `66f4a8c1…`, `cb33aec3…`, `90477716…`
- **Stato:** completato per la giornata; terzo sweep con l'immagine `6ed146ab…` (`build/f0/ciukios-hw.img`) da eseguire

## Contesto

Prima qualifica dell'immagine F2 su hardware reale (ThinkPad T23, disco
Transcend TS64GMSA230S scritto con `scripts/test/write_physical.sh`,
cattura seriale 38400 8N1 con `scripts/test/serial_capture.sh`). La
porta seriale del T23 va abilitata nel BIOS (3F8, IRQ 4); il collegamento
richiede un null-modem (TX/RX incrociati). I log restano in
`legacy/local/physical/`, mai pubblicati.

## Cosa cambia

1. **Sweep automatico (direttiva f1-28 e seguito).** `\SYSTEM\BOOT.CFG`
   accetta `probe=(f0|f1|f2|all):sweep run=<8hex> [step= state=]`; il
   loader lo usa al posto del tasto `P`; il kernel esegue una sonda per
   avvio, scrive il cursore prima di ogni passo e si riavvia da solo
   (impulso 8042 `0xFE`, poi INT3 su IDT vuota); dopo un panic o un
   blocco basta spegnere e riaccendere. Il runner importa la cattura
   multi-boot (`run.py f2-all --physical-capture <cartella>`, con
   `acquisition.json` e `f0.log`; `--image-sha256` per un'immagine
   storica). Le sonde che richiedono l'operatore mostrano un prompt di
   60 s e altrimenti registrano `not_run reason=operator_absent`
   (f1-31). Le scritture del cursore sono contabilizzate a parte così
   le sonde di sola lettura non le vedono (f1-32).
2. **Controller PS/2 del T23 (f1-27, f1-29).** Diagnostica per passo
   dell'inizializzazione; il controller incorporato risponde al test
   d'interfaccia `0xAB` con `0xFA`: i test `0xAB`/`0xA9` sono ora
   informativi e la disponibilità la decidono gli ACK del dispositivo.
3. **Stack del kernel a 16 KiB per task (f2-20).** Sul T23 la sonda
   `fd-table` andava in double fault; il percorso `vfs_open` →
   `fat_lookup` → `fat_next` con un'interruzione arriva a 8 208 byte;
   picco misurato dopo la modifica 8 356 byte; record `case=stack`.
4. **Fault su hardware (f2-22).** `fault-repair` consegnava 5 fault su
   7: la stampa dei record a 38400 baud consumava la scadenza
   dell'acknowledgement; ora gli snapshot sono bufferizzati e i gestori
   confermati prima della stampa. I registri di controllo erano corretti.
5. **Casi senza fixture su hardware (f1-30, f2-21).** `fat-read` senza
   dischi fixture e la coppia `exdev`/`readonly` di `fd-table` senza
   secondo volume valgono `not_run` con motivo esplicito.
6. **Recupero del bit dirty (f1-26)** confermato sul T23:
   `reason=dirty_recovered` all'avvio dopo uno spegnimento brusco.

## Verifiche

- Primo sweep (`44444444`, immagine `cb33aec3…`, 33 avvii): F0 tutto
  superato; F1 superato tranne `input`, `fat-read`, `safe`; F2 superati
  `elf-load`, `spawn-wait`, `mmap`, `threads-wait`, `libc-smoke`,
  100 cicli di `crash-isolation` con il desktop reale; `fd-table` in
  double fault.
- Secondo sweep (`66666666`, immagine `90477716…`, 35 avvii,
  `SWEEP_END passed=27 failed=6 not_run=2`): driver di input pronto,
  `fd-table` senza fault, `app-gate` superato su hardware (suite Lua
  5.4.8 e supplemento con uscita zero), `signals-fault` con il vero
  `#AC`; i fallimenti residui sono quelli corretti ai punti 1, 4, 5.
- Su QEMU, dopo ogni fusione, i casi toccati passano e il batch
  `f2-all` sull'immagine `5fe9446d…` chiude 186 casi su 191 (i tre
  `uart-absent-*` richiedono conferma a schermo, due casi sono fisici).
  Test host 219 OK. Dettagli per immagine in
  `docs/validation/2026-10-11-integrated-image-f0-regression.md`.

## Decisioni

- Un'unica immagine per tutte le macchine; lo sweep hardware è l'unico
  modo di eseguire la matrice T4 senza operatore, con prompt bounded
  per i soli casi che richiedono tasti o puntatore.
- I log fisici restano fuori dal repository; i test che li usano come
  fixture saltano quando il file manca.

## Prossimi passi

Terzo sweep sul T23 con l'immagine `6ed146ab…` (operatore presente ai
prompt), import e confronto; poi Armada E500 e un PC assemblato con lo
stesso procedimento.
