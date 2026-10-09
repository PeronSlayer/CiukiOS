# Prima generazione: loader UEFI e kernel x86_64

- **Data:** 2026-04-15 – 2026-04-21
- **Tipo:** retrospettiva
- **Versione/commit:** dal commit iniziale al 21 aprile 2026 (circa 420 commit)
- **Stato:** superato da [Reset verso BIOS legacy](2026-04-22-01-reset-bios-legacy.md)

## Contesto

Il progetto è nato con un loader UEFI (`boot/uefi-loader/loader.c`), un
kernel ELF64 in long mode e un obiettivo di compatibilità DOS 6.2.

## Cosa è stato fatto

- Loader UEFI, scaffolding di stage2, test di avvio automatici.
- Integrazione di OpenGEM attraverso un percorso di cambio modalità
  long mode → modalità protetta a 32 bit → V86 → ritorno, con TSS a 32 bit,
  GDT/IDT dedicate e gestore #GP in C.
- Primi servizi INT 21h, I/O file in V86 e passthrough dei vettori.

## Lezione

Il long mode non ha la modalità V86: ogni servizio DOS richiedeva un doppio
cambio di modalità, fragile (registri R8–R15 indefiniti dopo un task switch a
32 bit, puntatori persi). Questa complessità ha portato al reset del 22
aprile. È una lezione da tenere presente per qualsiasi futura revisione delle
fondamenta: un kernel protetto **a 32 bit** può ospitare V86 nativamente,
uno a 64 bit no.

## Riferimenti

- `git log --until=2026-04-21`
- Commit iniziale: "Initial commit: UEFI loader, stage2 scaffolding, and boot tests".
