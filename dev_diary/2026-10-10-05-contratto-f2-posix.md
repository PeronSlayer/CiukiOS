# Contratto F2: sottoinsieme POSIX, ABI nativa v1, accettazione

- **Data:** 2026-10-10
- **Tipo:** decisione (design)
- **Versione/commit:** commit di questa voce
- **Stato:** approvato; le direttive F2 partono da qui

## Contesto

La decisione di scope del 9 ottobre richiede, prima del codice F2, la
specifica del sottoinsieme POSIX, del modello dei processi e dell'SDK.
Codex (gpt-6-astra, ricerca web) ha scritto i tre documenti sotto la
direttiva f2-00; il lead li ha rivisti; Codex Sol ha fatto la revisione
incrociata (sette correzioni richieste, tutte applicate insieme a due del
lead).

## Decisioni

- **libc:** newlib 4.5.0.20241231 con port CiukiOS e libreria pthread
  propria; picolibc e musl scartati (motivazioni in `posix-subset.md`).
- **Syscall:** 53 nuove chiamate, numeri 16–68 (0–5 congelate da F0,
  6–15 riservate), con registri, semantica di blocco ed elenco completo
  degli errori; errno con la numerazione Linux i386; tipi ILP32 con
  `off_t`/`time_t` a 64 bit; record con dimensioni e offset fissati.
- **Processi:** ELF32 statico a 0x00400000 (profilo ristretto), `spawn`
  con eredità esplicita dei descrittori, niente `fork`/`exec`, `waitpid`,
  PID 1 supervisore; limiti: 64 processi, 256 thread, 16 per processo,
  128 fd.
- **Thread:** 1:1 con TLS tramite descrittore GDT (indice 7, selettore
  0x3B), estensione `wait_word`/`wake_word` (non futex) con generazioni
  di mappatura; mutex, condition e `pthread_once` ritentano su EINTR.
- **Segnali:** insieme ridotto, consegna con `ciuki_ucontext` (immagine
  FNSAVE), un solo handler attivo per thread, `sigreturn` validato;
  #PF/#GP → SIGSEGV, #UD → SIGILL, #DE/#MF → SIGFPE, #AC → SIGBUS.
- **Percorsi:** vista POSIX sul VFS a lettere: `/` = `C:\`, `/mnt/d`…`/mnt/z`,
  `/dev/null` e `/dev/console` sintetici; `PATH_MAX` 1040, `NAME_MAX` 765.
- **Orologi:** MONOTONIC dal timer; REALTIME seminato dal provider RTC in
  sola lettura di F1 se qualificato, altrimenti dall'epoca di build
  (`realtime_source`).
- **Desktop a ring 3:** oggetti superficie (XRGB8888 ≤ 16 MiB), `present`
  sincrono sul framebuffer F1, coda eventi di input (codici tasto set 1,
  anche per il decoder nativo set 2), canali di messaggi (256 B, 4 fd),
  grant di display/input installati solo dal supervisore.
- **SDK:** `build/tools/ciuki-sdk/` con `ciuki-cc`, sysroot, `crt0`,
  `libc/libm/libpthread/libciuki`, linker script, manifest con hash.
- **Gate:** Lua 5.4.8 con la suite ufficiale portabile (`_U=true`,
  archivio con hash pubblicato) eseguita nel guest, più uno script di
  test CiukiOS; `doomgeneric` come obiettivo opzionale.
- **Accettazione:** nove sonde `f2:` (`elf-load`, `spawn-wait`, `fd-table`,
  `mmap`, `signals-fault`, `threads-wait`, `crash-isolation`, `libc-smoke`,
  `app-gate`), suite `f2-process/runtime/desktop/app`, nessun alias nel
  kernel per F2; qualificazione hardware unica F0–F2 alla fine.

## Aperto

Dimensione misurata del port newlib, adattamenti al build con clang,
esito reale della suite Lua nel guest, comportamento F1 sui portatili,
requisiti futuri di Wine.
