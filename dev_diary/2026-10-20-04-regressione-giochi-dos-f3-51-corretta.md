# Giochi DOS di nuovo avviabili dal menu: regressione di f3-51 trovata per bisezione e corretta

- **Data:** 2026-10-20
- **Tipo:** regressione (VM DOS)
- **Versione/commit:** 0.9.2 pre-alpha / main `b2bc10a7` (correzione `85b9024a`); immagine QEMU `c3f7cadc65781850b2afb1ae384b0e3f238afc527b9d69b26691d917fd0a72ea`
- **Stato:** corretta e verificata su QEMU; seguiti pianificati

## Fatti

1. Sull'immagine 0.9.2 un gioco avviato da Programmi > Giochi restava a "Loading DOOM... Preparing..." (record `[launch-stage] name=dosbox stage=vm-create`, processo dosbox occupato nel kernel per oltre 4 minuti); la sonda `f3:vm-boot` passava (`event=END status=PASS`).
2. Bisezione automatica `git bisect run --first-parent` su QEMU (avvio di DOOM dal menu, giudizio sul dialogo "Starting program" dopo 150 s), con le correzioni di f0-44 e del build applicate ai commit intermedi: buone `597c0efa`, `04db814a`, `f6d36499`; cattiva `fe9a34ef` (fusione di f3-51).
3. Causa (diagnosi di un agente Claude Opus): lo SHA-256 dell'intera base del gioco, calcolato a ogni avvio dentro la chiamata di creazione (punto 8 di f3-51), leggeva il disco dei giochi da 27.336.704 byte 512 byte alla volta con un seek per settore; ogni lettura FAT ripercorre la catena dei cluster dall'inizio e ogni seek annulla la lettura anticipata: circa 1,8×10^8 consultazioni della FAT. I test host di f3-51 calcolavano l'hash di una base da 369 settori.
4. Correzione `85b9024a`: un solo seek e letture sequenziali da 64 KiB in un blocco non valido della cache della VM; stesso digest, stessi nomi degli overlay, stesso record `game_launch`; test `test_game_window_base_hash` (1 seek e 418 letture per 27 MB). Sull'host lo schema di lettura passa da 29,2 s a 0,25 s. Suite kernel, DOS (49 test), `run_hook_cpu.sh` e apps verdi.
5. Verifica su QEMU (TCG pentium3, 512 MiB): DOOM esce dal dialogo tra 60 e 90 s dopo il clic e la demo gira nella finestra; durante l'attesa il desktop mostra "Not responding". Cattura `docs/screenshots/0.9.2/12-doom.png`.

## Decisioni

1. Seguiti registrati nella direttiva f3-51 (emendamento 2): un cursore di cluster per file aperto nel percorso di lettura FAT (direttiva di storage) e la continuazione f3-51c, che conserva il digest per identità della base e lo ricalcola solo dopo una modifica.

## Riferimenti

- `docs/directives/f3-51-*.md` (emendamento 2); voci 2026-10-20-02, 2026-10-20-03.
