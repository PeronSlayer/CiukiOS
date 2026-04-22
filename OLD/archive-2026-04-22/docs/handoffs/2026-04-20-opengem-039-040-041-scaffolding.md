# Handoff 2026-04-20 — OPENGEM-039/040/041: compat-task + live-entry scaffolding

## Context and goal
Completare le fasi 039, 040 e 041 del design OPENGEM (documentato in
`docs/opengem-016-design.md`) sotto l'autorizzazione dell'utente
"procedi in autonomia fino al completamento di tutte le fasi, ti
autorizzo. dopo che arriviamo al risultato che si avvia opengem
correttamente allora faremo il merge" seguita da "fai il merge e poi
procedi in autonomia con le altre fasi".

Obiettivo finale di sessione: raggiungere uno stato in cui tutto lo
scaffolding per l'entry v8086 è staged, arm-gated, statically-gated,
mergeato a `main`, e il boot path resta intatto.

## Files touched
- `stage2/include/vm86.h`  — blocco OPENGEM-039 (compat-task image API),
  blocco OPENGEM-040 (compat-entry arm-gate API), blocco OPENGEM-041
  (live double-arm API con `vm86_compat_entry_enter_v86`).
- `stage2/src/vm86.c`       — implementazioni C di build/verify/probe
  per 039, wrapper arm-gated per 040, wrapper double-arm per 041.
- `stage2/src/vm86_compat_entry.S` (nuovo, 040) — trampolino staged con
  defensive `hlt` al prologo, body_live staged ma non raggiungibile.
- `stage2/src/vm86_compat_entry_live.S` (nuovo, 041) — trampolino live
  unguarded `vm86_compat_entry_enter_asm`, raggiungibile solo
  tramite `vm86_compat_entry_enter_v86()` con entrambi i magic.
- `scripts/test_vm86_compat_task.sh` (nuovo, 53 check) — gate 039.
- `scripts/test_vm86_compat_entry.sh` (nuovo, 52 check) — gate 040.
- `scripts/test_vm86_compat_entry_live.sh` (nuovo, 56 check) — gate 041.
- `scripts/test_vm86_gp_isr_real.sh` / `scripts/test_vm86_gp_isr_install.sh`
  — aggiunta emissione `[PASS]` footer per il regression runner.
- `Makefile` — target `test-vm86-compat-task`, `test-vm86-compat-entry`,
  `test-vm86-compat-entry-live`.

## Decisions made
1. **Una branch per fase** (user preference): 039, 040, 041 sviluppati
   su branch indipendenti chainati (039 off main, 040 off 039, 041 off
   main dopo merge di 039+040).
2. **Merge incrementale a main** appena verde, coerente col pattern già
   usato per 036/037/038. `main` è sempre buildabile e passa tutti i 25
   gate statici.
3. **Default disarmed ovunque**: ogni fase introduce un arm flag statico
   a 0 di default; ogni API critica richiede magic + flag.
4. **Magic constants disgiunti**: 039=`0xC1D39390u`, 040=`0xC1D39400u`,
   041=`0xC1D39410u`. Sentinel disgiunti: 039=`0x0390u`, 040=`0x0400u`,
   041=`0x0410u`.
5. **041 preserva 040 intatto**: il trampolino unguarded vive in un file
   asm separato (`vm86_compat_entry_live.S`). Il `hlt` difensivo nel
   file 040 non è stato rimosso. Il live path è esposto solo tramite
   `vm86_compat_entry_enter_v86()` che richiede entrambi i magic e
   entrambi i flag contemporaneamente.
6. **TSS.cr3 = 0 in 039**: la privileged `mov %cr3` è deferita al
   fill_frame di 041 (s_vm86_compat_tss.cr3 viene patchato lì) per
   non violare la gate 034 che scansiona `vm86.c` fino a EOF.
7. **Niente shell wiring in 041**: `enter_v86` è callable solo da codice
   di test — nessun comando shell attiva 040/041. La validazione
   runtime è esplicitamente deferita al milestone gem.exe loader.
8. **Hotfix 032 su main**: durante il merge 038 è emerso che
   `vm86-arm-live` in `shell.c` chiamava direttamente
   `vm86_idt_shim_build()` violando la boot-path isolation 032. Fix:
   la build è stata spostata dentro `vm86_gp_isr_install` e rimossa
   dal shell command. Commit `d618f1f` su main.

## Validation performed
- `make build/stage2.elf` verde dopo ogni fase.
- Regression 25/25 gate statici verdi (017..041) via
  `/tmp/run_gates2.sh`.
- Boot path non modificato: nessuna arm-flag default != 0, nessun
  nuovo caller di API pericolose da shell.c o da asm di fasi
  precedenti.
- Validazione runtime **non eseguita** in questo ambiente (QEMU
  non avviabile headless con il log serial previsto). L'utente ha
  precedentemente confermato che il boot funziona correttamente quando
  eseguito direttamente su host terminal — la superficie 039/040/041
  è invisibile al boot path quindi non dovrebbe impattare questa
  confidence esistente.

## Risks and next step
### Rischi principali
1. **Layout struct asm vs C**: `vm86_compat_entry_scratch_image` in
   `vm86.c` deve matchare byte-per-byte `vm86_compat_entry_scratch`
   in `vm86_compat_entry.S`. Se un futuro refactor aggiunge campi
   senza aggiornare entrambi, la live entry leggerà dati sbagliati
   e causerà triple-fault.
2. **TSS32.cr3 patch tempistica**: `fill_frame` in 041 patcha
   `s_vm86_compat_tss.cr3` a tempo di fill. Se qualcuno chiama
   `enter_v86()` senza aver prima chiamato `fill_frame()`, TSS.cr3=0
   e la prima task switch dentro il compat entry triple-faulta.
   Mitigato dal requisito di magic+flag su entrambe le API.
3. **EFLAGS=0x00023202**: VM=1 (bit17), IF=1 (bit9), reserved-1 bit
   (bit1). IOPL=0. Se qualcuno cambia questo literal in asm senza
   capirlo, triple-fault immediato.
4. **Unvalidated live path**: nessuna parte del trampolino è mai stata
   eseguita. Il primo tentativo runtime potrebbe rivelare problemi di:
   - GDT encoding (VM86_GDT_V86_TSS slot type = 0x09, non 0x0B
     availability — verificato in static gate ma non runtime);
   - alignment dell'ESP0 stack (staticamente aligned a 16 nell'asm);
   - selettori DS32/CS32/SS v86 (controllati in verify() statico);
   - PE/long-mode compat descriptor layout.
5. **Irreversibilità**: `enter_v86` non ritorna. Se la chiamata
   triple-faulta, la sola via di recovery è reboot.

### Next step: milestone gem.exe loader
Non è OPENGEM-042 — è il lavoro successivo allo scaffolding 037..041.
Scope proposto:
1. Aggiungere `COM/gem` builder che importi FreeGEM binaries
   (`gem.exe` + risorse) da `third_party/freedos/` o da un archivio
   FreeGEM sotto licenza idonea (vedi `docs/freedos-integration-policy.md`).
2. Scrivere un loader MZ v86-compatibile che mappi `gem.exe` a
   un linear address fisso (es. `0x10000`), costruisca PSP, riservi
   stack v86 a `0xFFFE` entro lo stesso segment.
3. Rimuovere il reject "MZ 16-bit" a `stage2/src/shell.c:4450` solo
   quando il loader è pronto, e gatare la rimozione dietro una flag
   runtime `vm86-gem-live-arm`.
4. Aggiungere comando shell `gem` che:
   a. arma 038, installa ISR, arma 039, builda compat-task;
   b. arma 040, chiama `prepare(host_cr3_real)`;
   c. arma 041, chiama `fill_frame(img, host_cr3, cs=seg(psp), ip=0x100, ss=seg, sp=0xFFFE, magic040, magic041)`;
   d. chiama `enter_v86(magic040, magic041)`;
   e. **(runtime-only)** handle `#GP` via ISR 038 → decode v86 opcode
      via decoder 034 → dispatch (INT 10h mode13 → gfx_modes, INT 21h →
      INT21h handlers già esistenti).
5. **Mandatory pre-wire runtime QEMU validation**:
   - Aggiungere comando shell `vm86-probe-041` che esegue solo
     `vm86_compat_entry_live_probe()` (non entra in v86 ma verifica
     scratch contents + arm gates). Deve stampare `probe complete`
     senza triple-fault su QEMU.
   - Solo dopo questo OK il test reale con `gem` può essere attivato.

### Riferimenti
- Design: `docs/opengem-016-design.md` §5.2 three-level model.
- Roadmap: `docs/roadmap-ciukios-doom.md`, `docs/roadmap-windows-dosbased.md`.
- Regression runner (non-tracked): `/tmp/run_gates2.sh` con 25 gate.
- Magic constants in uso: 035..041 (`0xC1D393(50..10)u`).

## Status sintetico
- ✅ OPENGEM-039 merged a main.
- ✅ OPENGEM-040 merged a main.
- ✅ OPENGEM-041 merged a main.
- ✅ 25/25 gate statici verdi.
- ✅ `make build/stage2.elf` verde su main.
- ⏳ Runtime QEMU validation: da eseguire manualmente dall'utente.
- ⏳ Shell wiring di `gem` e del loader MZ: milestone successivo.
