# Decisioni del proprietario: gestione di disco, memoria, cache e tempo reale completa sul modello Windows 98/ME; gate QEMU sospesi, immagini direttamente sull'hardware

- **Data:** 2026-10-19
- **Tipo:** decisione (architettura, processo)
- **Versione/commit:** 0.9.0 pre-alpha / main `3fbffcde`
- **Stato:** in vigore

## Contesto

Dopo la prova dell'immagine `82fadce3` (voce 2026-10-19-02) e la diagnosi dei problemi di copia, riproduzione MP3 ed effetti sonori nelle VM (direttive f1-82, f4-34, f2-85, f4-35), il proprietario ha chiesto come si intende risolverli e se thread e worker siano adeguati a una CPU come il Pentium III. La risposta del lead: pochi thread che dormono in attesa dell'I/O, come in Windows 98 e 2000 sulla stessa classe di macchine; i costi reali su un P3 sono ridisegni, copie di memoria e decodifica, non i cambi di contesto (circa 5000 al secondo oggi, intorno all'1 % della CPU).

## Decisioni

1. **Modello Windows 98/ME, completo.** Il proprietario dispone che la gestione di disco, memoria, cache e tempo reale di CiukiOS segua "pari pari" quella di Windows 98/ME: non soluzioni di base ma una gestione completa, non abbozzata. In termini di sottosistemi di riferimento: cache disco unificata a dimensione dinamica con scrittura ritardata e lettura anticipata (VCACHE), gestore di memoria virtuale con paginazione su richiesta, working set e file di scambio (VMM), sottosistema I/O a richieste asincrone con driver stratificati in modalità protetta e DMA bus master (IOS/ESDI_506), file system VFAT con IFS manager, priorità dei thread con boost dinamici per primo piano e completamento I/O, risoluzione dei timer e gestione degli interrupt (VMM/VxD). Prima del codice: ricerca sulle fonti primarie (Windows 98 Resource Kit, DDK di Windows 98, documentazione Microsoft su VCACHE, VMM e IOS, letteratura tecnica di riferimento) e un contratto di progetto con la tabella delle lacune rispetto al kernel attuale, revisionato in modo incrociato; poi direttive di implementazione per sottosistema. La direttiva di ricerca e contratto è `docs/directives/f0-42`.
2. **Gate QEMU sospesi.** Per il momento il proprietario non vuole i gate QEMU: l'immagine va consegnata direttamente per la prova sull'hardware reale. Restano in vigore: suite host per ogni consegna e sull'albero fuso, build canonica con albero pulito, immagine T23 con il suo SHA-256, diario, cattura seriale e log su disco come evidenza. La ripresa dei gate QEMU (smoke compreso) avverrà su richiesta del proprietario; fino ad allora un risultato QEMU non è richiesto per consegnare.
3. Le quattro direttive in corso (f1-82, f4-34, f2-85, f4-35) proseguono: la politica di writeback a budget di f1-82 e i worker di f4-34/f2-85 sono già nella direzione del punto 1.

## Riferimenti

- Voci 2026-10-19-01 e 2026-10-19-02; `docs/directives/README.md`; `AGENTS.md` (sezione Testing aggiornata con la sospensione).
