# Approvazione del proprietario: piano del Setup da CD per la distribuzione della 1.0 Beta

- **Data:** 21 ottobre 2026
- **Tipo:** decisione (distribuzione), piano
- **Versione/commit:** 0.9.2 pre-alpha / main `7b2d5f89`
- **Stato:** in vigore; direttive f9-01 e f9-02 avviate

## Fatti

Il proprietario ha chiesto, come attività indipendente dal programma in corso, di preparare l'installazione del sistema da CD per la distribuzione della 1.0 Beta. Il lead ha scritto il piano `docs/design/cd-setup-plan.md` e il mockup completo delle dodici schermate (640×480, identità Ciuki, procedura guidata di classe Windows 98) e il proprietario lo ha approvato.

## Decisioni

1. Il CD avvia il loader F0 invariato tramite emulazione El Torito di un'immagine FAT (metodo già provato dal CD della linea 0.8), perché l'estensione fissa del loader è piena (f0-54); il kernel monta il CD vero con il driver ottico e lancia `/Programs/setup` quando `BOOT.CFG` contiene `setup=1` (token riservato dal lead).
2. Gli strumenti disco (MBR, partizioni, formattatore FAT32 con la geometria canonica di `build_image.py`, installazione del loader, verifica) stanno in una libreria utente `sdk/libdisk` sopra nodi di dispositivo a disco intero con autorizzazione riservata al Setup; il kernel non ospita codice di sola installazione.
3. Sequenza: f9-01 (media e avvio) e f9-02 (strumenti disco) in parallelo; poi f9-03 (applicazione Setup) e f9-04 (copia e configurazione); poi f9-05 (primo avvio e console di ripristino) e f9-06 (qualificazione: installazione non presidiata end-to-end su QEMU come gate, poi T23). QEMU è ammesso per la suite `f9-cd-smoke` e per l'installazione non presidiata.
4. Il piano riceve una revisione incrociata di Codex in parallelo all'avvio delle prime due direttive; gli emendamenti si applicano alle direttive successive.

## Riferimenti

- `docs/design/cd-setup-plan.md`; `docs/directives/f9-01-cd-media-and-boot.md`, `f9-02-native-disk-tools.md`; voce 2026-10-09-06 (architettura), `docs/design/hardware-baseline.md`.
