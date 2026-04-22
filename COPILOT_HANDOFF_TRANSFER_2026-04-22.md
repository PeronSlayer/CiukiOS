# CiukiOS Handoff Transfer (PC Change)

Data: 2026-04-22
Repo: CiukiOS
Branch corrente: main
HEAD: d5e9707

## 1. Stato sintetico
- Dynamic RAM detection in stage1 implementata via BIOS INT 12h nel footer shell.
- Feature attiva solo su profilo floppy (FAT12) per rispettare il limite dimensionale stage1 (10240 byte).
- Profilo full (FAT16) mantiene footer senza path dinamico RAM per evitare overflow stage1.
- Ultimo commit pubblicato su origin/main: d5e9707.

## 2. Ultimi commit rilevanti
- d5e9707 stage1: dynamic RAM via INT 12h in footer (floppy only, FAT12)
- f759618 stage1: add system info (RAM/CPU/DISK) to footer right side
- 3773fd8 stage1: keep left-aligned shell text, remove only gray line
- 486a823 stage1: fix shell chrome reference after layout text removal
- 393e5df stage1: aggiorna shell layout tweaks

## 3. Verifiche eseguite (questa sessione)
Build:
- bash scripts/build_floppy.sh -> OK
- bash scripts/build_full.sh -> OK

Smoke test QEMU:
- bash scripts/qemu_test_floppy.sh -> PASS
- bash scripts/qemu_test_full.sh -> PASS

Esito: entrambe le immagini generano marker stage0/stage1 corretti.

## 4. File toccati nell’ultima feature
- src/boot/floppy_stage1.asm

Cambi principali:
- Footer shell: rendering di prefisso RAM + valore KB dinamico su FAT12.
- Conversione numero AX -> stringa decimale in buffer locale (convert_dec_buf).
- Conditional assembly per contenere footprint sul profilo full.

## 5. TODO operativo immediato (handoff target)
- Completare Phase 3 validation and gating.
- Preparare/chiudere bootstrap OpenGEM sequence.
- Mantenere vincolo stage1 <= 10240 byte in ogni modifica futura.

## 6. Procedura di bootstrap su nuovo PC
1. Clonare repo e checkout main.
2. Eseguire pull fast-forward:
   - git checkout main
   - git pull --ff-only origin main
3. Verifica rapida baseline:
   - bash scripts/build_floppy.sh
   - bash scripts/build_full.sh
   - bash scripts/qemu_test_floppy.sh
   - bash scripts/qemu_test_full.sh
4. Se tutto verde, riprendere dai TODO della sezione 5.

## 7. Note importanti per altri agent
- Non rimuovere i blocchi condizionali FAT12/FAT16 in stage1 senza ricontrollo size budget.
- Se il test seriale QEMU non mostra marker ma il boot locale funziona, classificare come possibile issue infrastrutturale di capture, non regressione certa del boot path.
- Evitare modifiche invasive non richieste in aree non correlate durante fix size-sensitive.

## 8. Prompt takeover consigliato
Sei un nuovo agent su CiukiOS. Parti da main a commit d5e9707.
Obiettivo immediato: completare la Phase 3 validation and gating senza regressioni.
Vincoli:
- stage1 deve restare <= 10240 byte
- build + smoke test devono restare verdi su floppy/full
- preservare dynamic RAM footer su FAT12
Output richiesto a fine batch:
- file modificati
- test eseguiti con esito
- rischi residui e prossimi passi

## 9. Stato git locale al passaggio
- Branch: main
- Sincronizzazione: in pari con origin/main al commit d5e9707
- Presenza file non tracciato locale: os
  - Verificare se è intenzionale prima di eventuale commit.
