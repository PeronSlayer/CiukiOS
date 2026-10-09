# SDK F2: header ABI, port di newlib 4.5.0, crt0, pthread, `ciuki-cc`

- **Data:** 2026-10-11
- **Tipo:** modifica
- **Versione/commit:** commit di questa voce
- **Stato:** completato sull'host; qualificazione nel guest `not_run` finché il kernel F2 non è integrato

## Cosa è stato fatto

- **`ciuki/abi.h`** (direttiva f2-01, Codex Sol): unica fonte dell'ABI
  pubblica F2 — 492 simboli (errno, costanti, numeri di syscall 0–68,
  record, tag auxv, codici tasto, limiti) con 251 asserzioni statiche su
  dimensioni e offset; test host con 687 controlli (nativo `-m32` e JSON
  generato dal target).
- **SDK** (direttiva f2-06, Codex Sol): build offline e pinnato di newlib
  4.5.0.20241231 (archivio SHA-256 `33f12605…`, tag `newlib-4.5.0`, commit
  `5e5e51f1`) con otto patch registrate e port di sistema CiukiOS
  (hook rientranti, `__getreent` da `GS:8`, lock ritargettabili sulla
  libreria pthread, `off_t`/`time_t` a 64 bit, errno Linux); `crt0`;
  `libpthread` con i layout del contratto e i retry su EINTR; `libciuki`
  con i 53 stub generati dall'header; `ciuki.ld`; wrapper `ciuki-cc` con
  self-test; manifest con hash di strumenti, sorgenti, patch e 1.198
  avvisi di licenza. Build pulito in 17 s, 6,7 MB installati; hello-world
  11.099 byte di testo, `libc_smoke` 80.908. `make sdk` esegue il build
  nello scope con limiti di memoria. Guida: `docs/sdk.md`.
- Archivi pinnati scaricati e verificati dal lead in `build/downloads/`:
  newlib, `lua-5.4.8.tar.gz` (`4f18ddae…`), `lua-5.4.8-tests.tar.gz`
  (`9581d5a7…`, uguale al valore pubblicato da Lua).

## Decisioni

- `ciuki/abi.h`, gli header dell'overlay SDK, `crt0`, `libciuki` e
  `libpthread` sono con licenza MIT, così programmi di qualsiasi licenza
  possono includerli e collegarli staticamente; il kernel resta GPL-2.0-only
  come eseguibile separato. Registrato in `posix-subset.md`.
- Il framing dei record applicativi (chiamata 3) è del controller nel
  kernel: il programma riporta solo campi; sequenza, identità e terminale
  li aggiunge il kernel (direttive f2-05/f2-09).
