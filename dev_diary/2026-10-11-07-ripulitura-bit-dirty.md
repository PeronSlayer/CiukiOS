# Decisione: ripulitura automatica del bit dirty al mount, dopo la prima prova sul T23

- **Data:** 2026-10-11
- **Tipo:** decisione (comportamento dello storage) e analisi hardware
- **Versione/commit:** immagine `b9c052cd…` (commit `ac20b25`); direttiva f1-26
- **Stato:** decisa dal proprietario, implementazione affidata a Codex (direttiva f1-26)

## Contesto

Prima prova dell'immagine F2 sul ThinkPad T23 con cattura seriale a
38400 8N1 (log in `legacy/local/physical/`, non pubblicato). Fatti
osservati:

- CPU `000006b1` (Pentium III Tualatin, famiglia 6 modello 11 stepping 1),
  512 MiB utilizzabili, 17 prenotazioni di registro, 15 funzioni PCI.
- Il loader riconosce la UART tramite il BIOS Data Area e la prova sul
  registro scratch; con la porta seriale disabilitata nel BIOS non
  trasmette nulla, con la porta abilitata (3F8, IRQ 4) trasmette loader e
  kernel.
- Il desktop ring-3 compare con il ritratto e la tagline.
- L'inizializzazione dell'input PS/2 nativo ha riportato
  `input result=failed error=-5` in tre avvii su quattro; nell'avvio
  riuscito la tastiera funzionava. Su QEMU il caso non si presenta. In
  attesa dei record della sonda `f1:input` per la direttiva.
- Dopo un avvio del desktop spento senza smontaggio, l'avvio successivo
  monta C: in sola lettura (`mode=ro`, `disk_log=unavailable error=-30`):
  il bit di spegnimento pulito resta azzerato e nessun percorso del kernel
  lo ripristina, quindi dopo uno spegnimento brusco il disco resta
  read-only finché l'immagine non viene riscritta.

## Decisione

Il proprietario decide la **ripulitura automatica del bit dirty al
mount**: se la scansione di mount non trova corruzione (nessun cluster
perso, nessuna divergenza tra le copie FAT, nessuna catena corrotta), il
kernel azzera il bit in modo durevole su tutte le copie FAT e monta il
volume in lettura/scrittura, registrando `reason=dirty_recovered`; se la
scansione trova qualcosa, o se è impostato il bit di errore hardware, il
volume resta in sola lettura come oggi. La regola vale come
comportamento predefinito almeno per la fase di diagnosi su hardware e
viene scritta nei contratti `f1-acceptance.md` e
`vfs-storage-contract.md`; i casi `mount-dirty` e `mount-crash-reboot`
delle suite cambiano di conseguenza (direttiva f1-26).

## Motivazione tecnica

Il contratto F1 chiedeva che un volume dirty forzasse la sola lettura
per proteggere i dati, ma senza un percorso di recupero il sistema non è
utilizzabile su hardware reale dopo il primo spegnimento senza
smontaggio. La scansione di mount già calcola cluster persi,
corruzione delle catene e divergenza delle copie FAT: quando questi
indicatori sono tutti a zero, azzerare il bit è la stessa operazione
che il kernel compie allo smontaggio pulito. Il bit di errore hardware
resta escluso dal recupero automatico.

## Prossimi passi

Consegna di Codex su f1-26, revisione, nuova immagine, ripetizione del
batch `f2-all` su QEMU e delle prove sul T23 (sonde `f0:all`, `f1:all` e
F2 con cattura seriale).
