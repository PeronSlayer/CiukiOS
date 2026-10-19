# Revisione incrociata del contratto "gestione delle risorse di classe Windows 98/ME" e sequenza delle direttive di implementazione

- **Data:** 2026-10-19
- **Tipo:** decisione (architettura), analisi
- **Versione/commit:** 0.9.0 pre-alpha / main `47b6ea6f` (fusione di f0-42)
- **Stato:** contratto accettato; prime direttive di implementazione avviate

## Contesto

In esecuzione della decisione del proprietario (voce 2026-10-19-03), Codex (gpt-6.1-sol, xhigh, con ricerca in rete) ha consegnato in poche ore la direttiva f0-42: registro delle fonti P01–P27 (Resource Kit e DDK di Windows 98, articoli KB su VCACHE, swap e IOS, documentazione DDK di IOS, VxD, IFSMgr e dei timeout, Pietrek, Oney, Schulman, King, Mitchell, Russinovich/Cogswell, specifiche Intel BM-IDE, FAT e SDM, guida Creative), registro delle evidenze fisiche delle prove sul T23, tabella di 24 lacune (8 P0, 15 P1, 1 P2) suddivise in storage/cache/filesystem (8), memoria (6), scheduler/tempo reale (6), integrazione DOS (4), contratto proposto `docs/design/win98-class-resource-management.md` con criteri di accettazione C1–C5, M1–M4, R1–R3, D1 e una sequenza di 12 direttive (3 M, 5 L, 4 XL). L'audit corregge anche la diagnosi di f1-82: `write_block` e `barrier` rilasciano il lock della cache durante l'I/O; restano l'espulsione sincrona dei blocchi sporchi, la barriera a 5 s per dispositivo e la prenotazione del volume che copre il chiamante.

Sette domande di ricerca restano aperte perché i testi originali completi (capitolo del Resource Kit, DDK completo, supplementi VCache.doc e IOS_Port_DriverFAQ.doc, capitoli di Oney e Schulman) non sono raggiungibili dai percorsi consentiti: soglie di crescita e recupero della cache, età dei dati sporchi e scrittura ritardata sui supporti rimovibili, `ReadAheadThreshold`, coerenza dei file mappati, campionamento della sostituzione delle pagine e crescita dello swap, entità e decadimento dei boost, garanzie temporali di VTD/VPICD.

## Decisioni

1. Il contratto è accettato come progetto vincolante (sezione "Lead cross-review" in coda al documento). I numeri dei criteri sono politica CiukiOS finché una fonte primaria non stabilisce il valore di Windows 98/ME; le implementazioni procedono sui meccanismi e i valori vengono rivisti all'arrivo delle fonti. Al proprietario viene chiesto se dispone dei supporti originali del DDK e del Resource Kit di Windows 98.
2. Non adottati: scheduling cooperativo Win16, caricamento binario dei VxD e ABI INT 20h, esaurimento dell'arena di sistema e protezione permissiva, cache di VREDIR/VSERVER.
3. Restano: quanto ordinario di 10 ms e politica sensibile a 2 ms (non i 20 ms storici), obiettivo di 100 µs con interrupt disabilitati (le pause del firmware si attribuiscono, non si nascondono), anticipo audio nominale di f4-32 e politica della linea temporale di f4-35.
4. C1 (budget dei dati sporchi 12,5 %, età 1 s) è la politica di f1-82; C3 (equità dei lettori sullo stesso volume) e C2/C5 (pubblicazione FAT a stadi) seguono in f1-83 e f1-84. Supporti rimovibili in write-through fino alla chiusura delle fonti (f1-85).
5. Ordine: f1-82 (in corso) → f0-43 (registro unico delle risorse, soglie di pressione, token di pin) e f0-45 (priorità effettive, boost con scadenza, boost da completamento I/O, dispatch anti-starvation, donazione di priorità sui mutex) in parallelo su file disgiunti → f0-44 → f0-46 → f1-83 → f1-84 → f1-85 → f0-47 → f0-48 → f0-49 → f3-46. Ogni passo entra in `main` dopo le suite host e la build canonica; l'accettazione fisica avviene sul T23 per immagine, con i gate QEMU sospesi.

## Riferimenti

- `docs/design/win98-class-resource-management.md`, `docs/validation/f0-42-resource-management-audit.md`, `build/codex/f0-42-report.md` (locale); direttive `docs/directives/f0-42`, `f0-43`, `f0-45`.
