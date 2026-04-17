# Handoff: SR-EDIT-001 (`CIUKEDIT.COM`)

Date: 2026-04-17
Branch: `feature/copilot-sr-edit-001`
Baseline: CiukiOS Alpha v0.7.1

## 1. Context and Goal
Implementare un clone minimale line-oriented di EDIT come binario nativo `CIUKEDIT.COM`, usando esclusivamente le API INT 21h gia presenti in stage2, con supporto a create/open/save di file testo e marker deterministici.

## 2. Files Touched
Nuovi file:
- `com/ciukedit/linker.ld`
- `com/ciukedit/ciukedit.c`
- `scripts/test_ciukedit_smoke.sh`
- `docs/sr-edit-001.md`
- `docs/handoffs/2026-04-17-copilot-sr-edit-001.md`

File modificati:
- `Makefile`
- `run_ciukios.sh`
- `Roadmap.md`

## 3. Decisions
- Editor line-oriented (non full-screen), coerente con la superficie INT 21h attuale.
- Buffer statico in COM image: massimo `200` righe, `128` caratteri per riga.
- Normalizzazione newline in scrittura a `\n` deterministico (lettura compatibile anche con `\r\n`).
- Exit code taxonomy:
  - `0x00` clean quit (`:q` / `:wq` su successo)
  - `0x01` errore save/open/read/write
  - `0x02` errore parse/command handling
- Nessuna estensione INT 21h aggiunta in stage2.

## 4. Validation
Comandi richiesti dal task pack eseguiti in ordine:
1. `make clean all`
2. `make -C boot/uefi-loader clean all`
3. `bash scripts/test_ciukedit_smoke.sh`
4. `make test-stage2`
5. `make test-mz-regression`
6. `make test-m6-pmode`
7. `make test-vga13-baseline`
8. `make test-m6-dpmi-ldt-smoke`
9. `make test-m6-dpmi-mem-smoke`
10. `make test-doom-boot-harness`
11. `make test-doom-target-packaging`

Stato osservato:
- Build/lint locale del codice nuovo: `PASS`.
- In questo host, i gate runtime basati su boot QEMU mostrano log troncati al punto `Starting QEMU...` (nessuna marker tail conclusiva disponibile nei log raccolti in `.ciukios-testlogs/`).
- Per `test_ciukedit_smoke.sh` e altri smoke M6 esiste fallback statico nel test script; il comportamento runtime completo resta da chiudere su host con serial/runtime capture stabile.

Tail output disponibili (ultime righe catturate):

`make clean all`
```text
ld.lld -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_mem_smoke/linker.ld -o build/CIUKMEM.EXE.elf build/obj/com/ciukmem.o
llvm-objcopy -O binary build/CIUKMEM.EXE.elf build/CIUKMEM.EXE.payload.bin
build/tools/mkciukmz_exe build/CIUKMEM.EXE.payload.bin build/CIUKMEM.EXE
```

`make -C boot/uefi-loader clean all`
```text
objcopy \
    -j .rodata \
    -O efi-app-x86_64 \
    build/loader.so build/BOOTX64.EFI
```

`bash scripts/test_ciukedit_smoke.sh`
```text
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] Starting QEMU...
[CiukiOS] QEMU serial sink: file:/home/peronslayer/Desktop/CiukiOS/.ciukios-testlogs/ciukedit-smoke-serial.log
```

`make test-stage2`
```text
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] Starting QEMU...
[CiukiOS] QEMU serial sink: file:/home/peronslayer/Desktop/CiukiOS/.ciukios-testlogs/stage2-boot-serial.log
```

`make test-mz-regression`
```text
(output tail non disponibile nei log host durante questa run)
```

`make test-m6-pmode`
```text
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] Starting QEMU...
[CiukiOS] QEMU serial sink: file:/home/peronslayer/Desktop/CiukiOS/.ciukios-testlogs/m6-pmode-contract-serial.log
```

`make test-vga13-baseline`
```text
(output tail non disponibile nei log host durante questa run)
```

`make test-m6-dpmi-ldt-smoke`
```text
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] Starting QEMU...
[CiukiOS] QEMU serial sink: file:/home/peronslayer/Desktop/CiukiOS/.ciukios-testlogs/m6-dpmi-ldt-smoke-serial.log
```

`make test-m6-dpmi-mem-smoke`
```text
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] Starting QEMU...
[CiukiOS] QEMU serial sink: file:/home/peronslayer/Desktop/CiukiOS/.ciukios-testlogs/m6-dpmi-mem-smoke-serial.log
```

`make test-doom-boot-harness`
```text
[CiukiOS] OpenGEM inclusion status: SKIPPED
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] QEMU launch skipped (CIUKIOS_QEMU_SKIP_RUN=1)
```

`make test-doom-target-packaging`
```text
[CiukiOS] OpenGEM inclusion status: SKIPPED
[CiukiOS] Preparing OVMF_VARS...
[CiukiOS] QEMU launch skipped (CIUKIOS_QEMU_SKIP_RUN=1)
```

## 5. Risks and Next Step
Rischi residui:
- Validazione runtime end-to-end dei marker `[edit] open/save/quit` non completamente dimostrata su questo host per via della cattura runtime QEMU non conclusiva durante la sessione.

Next step suggerito:
1. Rieseguire la sequenza Validation su host CI/staging con serial capture stabile, confermare marker runtime CIUKEDIT e aggiornare il handoff con tail conclusivi `PASS` per tutti i gate.
