# JavaScript di NetSurf riattivato; EMMXXXX0 e memoria superiore nella finestra DOS

- **Data:** 2026-10-25
- **Tipo:** modifica, decisione
- **Versione/commit:** main `bdad5c24` (NetSurf), `744d0eb5` (DOS); non inclusi nell'immagine T23 `2287114d…`
- **Stato:** completato su host e QEMU; prova T23 da fare con la prossima immagine

## Contesto

L'immagine del mattino aveva JavaScript spento di default in NetSurf: lo
script di example.com mandava il programma in crash leggendo da un oggetto
nullo (`duk_hobject_find_entry_tval_ptr_stridx`, indirizzo 0x200).
Wolfenstein 3-D partiva, ma vedeva solo XMS. Nella finestra DOS restavano
circa 575 KB liberi.

## Cosa cambia

- **NetSurf, causa del crash.** Duktape usa valori impacchettati in 8 byte:
  i tipi intero, undefined, null, booleano e puntatore sono codificati come
  NaN di segnalazione. Il target i686 è compilato senza SSE (`-mno-sse`) e
  copia questi valori con `fldl`/`fstpl` della x87. Il caricamento x87
  trasforma un NaN di segnalazione in un NaN quieto, quindi un intero
  diventa un oggetto con un puntatore falso.
- **NetSurf, correzione.** La patch 0014 sceglie i valori non impacchettati
  di Duktape su x86 a 32 bit. Il test host `duktape_values_test.c`, compilato
  `-m32` con le opzioni dell'SDK, fallisce prima della patch e passa dopo.
  JavaScript torna attivo di default. Stesse patch:
  - 0015: CSS `light-dark()` usa il colore chiaro;
  - 0016: i file `.js` locali sono serviti come script.
- **Finestra DOS, EMS.** `CIUKIEMM.SYS` (`src/com/dosemm.asm`) fornisce il
  dispositivo a caratteri `EMMXXXX0`. Serve al rilevamento EMS con il
  metodo dell'handle aperto (LIM 4.0, appendice B), quello usato da
  Wolfenstein 3-D. Le risposte IOCTL sono come EMM386/Jemm. Il driver si
  installa solo se INT 67h ha già un gestore EMS funzionante.
- **Finestra DOS, memoria superiore.** Il profilo del fork usa
  `DOS=UMB,LOW` e carica SHARE in alto (`LH`).
- **Contratto f3-56.** I controlli del ciclo di vita accettano blocchi di
  processo (PSP, MCB, JFT) nella finestra UMB pubblicata dal VM
  (C9000h–E0000h). Tutte le strutture lette dal bind finale restano
  convenzionali.
- **Rete DOS.** I buffer IPX e le pile delle callback accettano la stessa
  finestra.

## Decisioni

- **Valori non impacchettati di Duktape su x86 a 32 bit.** Costo misurato:
  heap più grande. Nel modello a 32 bit i built-in passano da 64,9 KB a
  75,2 KB.
- **Emendamento a f3-56: processi ammessi in UMB.** Una configurazione utente
  con `DEVICEHIGH`, `FILESHIGH`, `LASTDRIVEHIGH` o `DOSDATA=UMB` metterebbe in
  UMB strutture lette dal bind, che le rifiuta. Il limite è documentato in
  `docs/design/dos-dpmi-contract.md`.

## Verifica

- **Host:**
  - suite NetSurf 38 test verdi;
  - `run_dos_memory.sh`, `run_unified_binding.sh` (sezione H56-07),
    `run_unified_file.sh`, test unitari `tests/host/dos` (89),
    `dosvm_test`, `tests/host/vm/run.sh`.
- **Revisione** Opus della modifica DOS: fusione dopo piccole correzioni,
  applicate.
- **Memoria misurata** (replay host): blocco per un programma al prompt da
  574.880 a 584.992 byte; SHARE in UMB.
- **QEMU (KVM):**
  - NetSurf scarica ed esegue lo script di example.com senza crash: errore
    di sintassi atteso, perché lo script usa ES2015;
  - Wolfenstein 3-D mostra EMS in uso e gioca il livello dimostrativo;
  - DOOM e Quake partono e si chiudono;
  - `exit` chiude la finestra DOS.
- **Non provato:** il T23.

## Riferimenti

- `docs/design/netsurf-port-plan.md`
- `docs/design/dos-dpmi-contract.md` (sezione 2026-10-25)
- `docs/design/unified-dos-contract.md`
- LIM EMS 4.0 Appendix B; id Software `ID_PM.C`; Duktape
  `misc/clang_aliasing.c` e l'opzione `DUK_USE_PACKED_TVAL`
