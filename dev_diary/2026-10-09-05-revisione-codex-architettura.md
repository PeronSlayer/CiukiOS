# Revisione incrociata di Codex sulla proposta di architettura

- **Data:** 2026-10-09
- **Tipo:** analisi
- **Versione/commit:** nessuno (sola lettura, nessun build o QEMU)
- **Stato:** completato; qualifica la [proposta](2026-10-09-02-revisione-architettura.md); superata nella raccomandazione dalla [decisione](2026-10-09-06-decisione-fondamenta-32bit.md)

## Contesto

Primo lavoro comune Claude–Codex
([metodo](2026-10-09-04-lavoro-in-simbiosi-claude-codex.md)).
Codex `gpt-6-astra`, effort `xhigh`, in sola lettura (circa 120.000 token),
ha confrontato la proposta con il codice reale.

## Esito

**Raccomandazione di Codex:** tenere Jemm e stabilizzare partendo dalle
prove; trattare un VMM proprio come progetto separato, da giustificare con un
prototipo delimitato.

### Affermazioni della proposta corrette da Codex (verificate da Claude sul codice)

- **"Memoria bassa condivisa senza proprietario": sbagliato.** `vmm_create` in
  `src/vm/session_vmm.inc` copia la memoria convenzionale in pagine private
  per ogni VM e costruisce le PTE per VM. Restano possibili puntatori
  ereditati non validi e mappature superiori/estese condivise.
- **"Il kernel in modalità reale possiede tutto": impreciso.** Sotto Jemm il
  DOS e la shell girano in V86, mentre CVSESSION gira a ring 0; INT 13h può
  già arrivare al driver ATA nativo (`src/vm/session_disk.inc`).
- **"Nessuno scheduler con priorità": falso.** `vm_scheduler_irq`
  (`src/vm/session_scheduler.inc`) serve IRQ0 indipendentemente dal CLI
  virtuale del guest, e l'audio urgente ha priorità in `session_vmm.inc`.
  Restano deboli i confini di scheduling e la latenza con interrupt
  disabilitati.
- **S3:** esistono già qualificazione, gating delle capacità e rollback
  (`src/vm/session_gpu_savage.c`); essere un JLM non spiega una copia fallita.
- **Processi nativi:** `src/vm/session_native_process.inc` offre già un primo
  processo isolato a ring 3 con page directory privata.
- **Il report del 9 ottobre** lascia esplicitamente aperte le cause di DA, S3
  e audio: la proposta le aveva trasformate in conclusioni causali.

### Lacune del piano F0–F4 segnalate da Codex

- Il modello DPMI va scelto **prima** di F0: l'adattatore HDPMI attuale cambia
  CR3 e dipende dalla cooperazione dell'host.
- Mancano A20/HMA, la sostituzione di XMS/EMS, il passaggio della memoria
  all'avvio, la proprietà del PIC con EOI e IRQ PCI condivisi.
- "VBE LFB sempre funzionante" è troppo forte: il modo va impostato prima di
  entrare in modalità protetta oppure serve un meccanismo serializzato per
  le chiamate al BIOS.
- Mancano formato eseguibile, ABI delle chiamate di sistema, FPU e pulizia
  dei processi; mancano politica ACPI/APM, controller ESS esatto dell'E500 e
  log seriale che funzioni prima dei driver disco.

### Codice riusabile secondo Codex

Riusabili quasi senza modifiche: `virtual_vga.c`, `guest_peripherals.c`,
`vga_x86.c`, `vga_presenter.c`, `session_audio.c`. La logica registri di
GPU/ATA è riusabile ma richiede nuovi servizi di mappatura, sincronizzazione e
tempi. `session_video.c`, l'assembly dello scheduler/JLM, gli adattatori
HDPMI, `src/runtime/`, SHELL e LFN richiedono port sostanziali.

### Alternativa più piccola proposta da Codex

1. Rendering software come base su hardware reale; accelerazione S3 opt-in e
   correzione della copia S3 separata.
2. Tracciare DA alla creazione padre/figlio, al ripristino dei vettori,
   all'entrata/uscita HDPMI e al teardown; verificare il percorso di
   ripristino di VMFORK (`src/com/vmfork.asm`) prima di sostituire il fork.
3. Misurare separatamente i buchi di servizio audio, gli intervalli con
   interrupt disabilitati, i tempi di risposta del guest e la calibrazione.
4. Poi estendere in modo incrementale i processi nativi già esistenti.

## Posizione di Claude dopo la revisione

La diagnosi della proposta era esagerata nei punti sopra e va considerata
superata su quei punti. Restano validi, e Codex non li contesta: la
dipendenza dal BIOS a runtime sull'hardware reale, i margini esauriti (HDPMI,
app a 64 KB) e il costo di mantenere 16 patch su codice di terzi. Codex stesso
ritiene un VMM proprio giustificato se spazi di indirizzamento indipendenti e
processi nativi sono requisiti del prodotto: è una decisione del proprietario.

## Decisione richiesta al proprietario

Scegliere tra:
- stabilizzazione guidata dalle prove (proposta di Codex), con il reboot
  rimandato a un prototipo;
- reboot delle fondamenta, con la proposta corretta secondo le lacune sopra.
