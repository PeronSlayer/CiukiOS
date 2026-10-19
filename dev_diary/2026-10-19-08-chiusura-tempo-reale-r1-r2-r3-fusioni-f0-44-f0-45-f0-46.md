# Chiusura del tempo reale del contratto Windows 98/ME (R1, R2, R3): fusioni f0-44, f0-45 e f0-46 con l'emendamento sul recupero dell'orologio delle VM DOS

- **Data:** 2026-10-19
- **Tipo:** integrazione (kernel: interrupt, lavoro differito, timer, priorità, cicli limitati)
- **Versione/commit:** 0.9.0 pre-alpha / main `3bbc9692` (fusioni `8ae026a6` f0-45, `4d78e724` f0-44, `3bbc9692` f0-46)
- **Stato:** integrato su main; qualificazione hardware rinviata (mandato 2026-10-19-07: nessuna immagine intermedia)

## Fatti

1. **f0-45 (R2, priorità effettive).** 32 livelli con base, boost e priorità ereditata e reinserimento in coda; foreground +2 per 20 ms, completamento I/O +1 per un quanto, dispatch anti-inedia a 100 ms, donazione transitiva sui kmutex con rilevamento dei cicli; quanti 10/2 ms invariati. Replay host: input→UI 2 ms, attesa normale ≤ 100 ms, budget dispositivo mantenuto; costo della politica 2,9× sul modello host.
2. **f0-44 (R1/M06, cicli dipendenti dalla dimensione).** Inventario di ogni ciclo kernel dipendente dalla dimensione con modello di costo e massimo fisico; punti sicuri o transazioni limitate per teardown, uscita dei processi, recupero heap, catene FAT, scansioni della cache e raccolta VGA; replay host con pulizia lunga, catene frammentate e cache tutta sporca. Restano: revoca/sostituzione atomica MMIO al tetto di 120 MiB, scansioni di frammentazione del PMM, interni dei pin.
3. **f0-46 (R1/R3, interrupt e timer).** Metà superiori che solo acquisiscono e notificano: massimi modellati con IF=0 ATA 18 µs, UHCI/OHCI 24 µs, AC'97 14 µs, ES137x/SB16 16 µs, NIC 5–8 µs (involucro comune 12 µs), i8042 nativo 17 µs, PIT 63 µs con 1.000 timer, dispatcher condiviso 40 µs; budget software 100 µs con registrazione separata di grezzo/addebitato/firmware/non classificato (la pausa IRQ 11 del T23 resta riportata grezza: 2.120 µs = 20 µs di lavoro + 2.100 µs firmware). Anello di lavoro differito a 64 voci con coalescenza contata (2.000 notifiche identiche → 2 turni), turni ≤ 1 ms. Coda dei timer AVL O(log n) al posto della scansione di tutti i task a ogni tick: lotto IRQ di 2 voci, turno di trasferimento 12 µs con 10 timer e 40 µs con 1.000, cancellazione sicura per generazione (1.000 timer, wrap a 64 bit, riarmo in callback, tick persi). Replay combinato con 1.000 timer: risveglio→esecuzione del refill audio 410 µs, servizio 500 µs, turno differito 80 µs; numeri della politica audio invariati.
4. **Emendamento 1 di f0-46.** Il recupero di 40 ms dell'orologio delle VM DOS (auto-init piccolo) passava in un solo callback da 18.425 µs; ora è una continuazione preallocata e sicura per generazione: 80 turni da ≤ 251 µs (massimo callback 248 µs, avanzamento audio per VM 246 µs), lavoro aggregato 19.355 µs; periodo, buffering, cadenza DMA f3-40 e politica f4-35 invariati.
5. **Fusione.** Conflitto in `tests/host/kernel_latency_test.c`: la prova dei 1.360 task di f0-44 contava le visite della scansione per tick rimossa da f0-46; ora arma i 1.360 dormienti nella coda dei timer e verifica il lotto IRQ ≤ `KTIMER_IRQ_BATCH` e i risvegli differiti (2 tick). Suite host kernel e audio verdi su main `3bbc9692`.

## Decisioni

1. R1, R2 e R3 del contratto `docs/design/win98-class-resource-management.md` sono considerati implementati sul lato host; la qualificazione sul T23 avviene con l'immagine finale del programma.
2. La pausa grezza IRQ 11 di 2,1 ms del T23 (riga R02 dell'audit f0-42) resta aperta come misura hardware: nessun rilassamento del budget software.
3. Sequenza successiva del contratto: f1-83 (C3, lanciata) → f1-84 → f1-85 → f0-47 → f0-48 → f0-49 → f3-46.

## Riferimenti

- `docs/directives/f0-44-*.md`, `f0-45-*.md`, `f0-46-*.md` (emendamento 1); `build/codex/f0-46-report.md`; voci 2026-10-19-04 e -07.
