# Integrazione notturna: dispatch F1, driver, processi F2, segnali, BIOS VM, Lua

- **Data:** 2026-10-11
- **Tipo:** modifica
- **Versione/commit:** da `cbc2ce8` (f1-03) a `997b157` (f1-07b)
- **Stato:** integrato su `main`; regressione F0 superata sull'immagine `d2daced5…`; sonde F1/F2 non ancora eseguite su QEMU

## Cosa è stato fatto

Tutte le implementazioni sono di Codex; il lead ha preparato le fusioni con
conflitti, rivisto i diff, eseguito test host e QEMU e integrato.

- **f1-03** (Sol): parser del selettore senza effetti collaterali, dispatch
  per fase in `probes_main`, registrazione delle sonde F1 tramite sezione
  `.f1probes` del linker (`CIUKI_F1_PROBE`), opzioni di `BOOT.CFG` a token
  interi con `SELECT_ERROR` sui token malformati, digest calcolati dal
  runner al posto del trasporto di fixture nel guest; 54 test host.
- **f2-02** (Astra): processi separati dai task con PID 1 supervisore,
  loader ELF32 ristretto con rollback, `spawn` con eredità esplicita e
  stack auxv, arena mmap con crediti e diritti massimi, thread con TLS su
  descrittore GDT 7 ricaricato a ogni switch, wait word con generazioni;
  94.456 controlli host; la sonda `protection` di F0 verifica ora il
  descrittore TLS.
- **f2-04** (Astra): segnali (dispositions, maschere, consegna con frame di
  320 byte e `ciuki_ucontext` con immagine FNSAVE, `sigreturn` validato,
  mappatura delle eccezioni, CR0.AM), 4.344 controlli host; hook per il
  payload NASM e per SIGCHLD applicati all'integrazione.
- **f2-07** (Sol): Lua 5.4.8 non modificato compilato con `ciuki-cc`,
  script `ciuki-f2.lua`, 50 payload con hash nell'immagine (`/bin/lua`,
  `/bin/hello`, `/bin/libc_smoke`, suite di test upstream, licenze) e
  rilettura T1 dal disco; suite portabile eseguita sull'host come solo
  riferimento (`final OK`).
- **f1-07 + f1-07b** (Astra): monitor V86 con decodifica limitata,
  PIC/PIT/RTC virtuali, VM BIOS serializzata con lease dell'input firmware,
  reflection degli IRQ, drain INT 16h, mouse INT 15h/C2h con callback,
  decoder set 1; il monitor osserva i byte letti dal firmware dalla porta
  60h per ricavare le transizioni dei tasti; workspace firmware
  C0000–EFFFF scrivibile solo nella VM; verifica sui sorgenti SeaBIOS
  rel-1.16.3 pinnati; 71.160 controlli host.
- **Evidenza QEMU** sull'immagine `d2daced5…` (build pulito `2b6e7cc`,
  kernel 722 KB, SDK 17 s, Lua 2,3 s, 50 payload verificati): `f0-smoke`
  2/2, `f0-core` 56/56, `f0-panic` come da record F0; `f1-input` ha
  ripetuto 59 regressioni F0 e si è fermata al caso UART-assente (conferma
  visiva) lasciando le 11 prove F1 `not_run`: regola delle precondizioni da
  correggere in f2-09. Record: `docs/validation/2026-10-11-integrated-image-f0-regression.md`.

## Decisioni

- Il controllo del frame V86 precede l'hook di consegna dei segnali in
  `trap_return`: i frame real-mode non raggiungono il percorso nativo.
- `codex exec` va lanciato con stdin da `/dev/null`: altrimenti legge il
  prompt anche dallo stdin del harness e resta bloccato all'avvio
  (registrato in AGENTS.md).
