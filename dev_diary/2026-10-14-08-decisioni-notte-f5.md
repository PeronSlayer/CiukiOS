# Decisioni del proprietario della sera del 14 ottobre (DOS a schermo intero, Win16, MIDI, font, gate F5)

- Data: 2026-10-14
- Tipo: decisioni
- Riferimenti: `docs/design/win32-personality-contract.md` (revisione incrociata e sezione decisioni), `docs/design/research/*-2026-10-14.md`

## Decisioni

- **Sessione DOS a schermo intero sull'hardware VGA** (modello Windows 98: il desktop rilascia la concessione dello schermo, Alt+Invio alterna finestra e schermo intero, Alt+Tab torna al desktop): approvata. Richiede l'emendamento di `device-firmware-ownership.md` che consente alle sessioni DOS a schermo intero le chiamate mediate al BIOS video per i modi VGA standard; direttiva f3-20.
- **Win16**: sottosistema Win16 completo subito dentro F5 (esecuzione NE reale su DPMI/V86), non la sola sostituzione degli installer a 16 bit; direttiva f5-19 con contratto proprio prima del codice.
- **Banco MIDI**: entrambi i banchi; il sistema rileva la RAM e sceglie da solo: banco ridotto (≤ 32 MiB) a 128 MiB, FluidR3 completo dove la RAM lo consente con sicurezza.
- **Font**: FreeType ovunque, anche nel percorso nativo (supera la scelta stb_truetype di `fonts-contract.md`); licenza FTL registrata; direttiva f2-63.
- **Gate di giocabilità F5**: dieci minuti anche su QEMU, una volta per immagine, di notte.
- **DLL di Wine**: se l'audit f5-14 conferma che la chiusura delle dipendenze costa più di quanto rende, CiukiOS scrive più DLL in proprio (LGPL-2.1-or-later) e il riuso di Wine si limita a moduli autonomi.
- **Titoli di accettazione F5** (demo e shareware liberamente distribuibili): in un disco giochi separato opzionale per i test, non nell'immagine di sistema.
- **Immagine**: una sola immagine finale con tutto implementato; nessuna immagine intermedia.

## Rapporto con le ricerche

Le ricerche del 14 ottobre (latenza del kernel, video delle VM DOS, strato Win32) sono state tradotte nelle direttive f0-21, f3-19, f5-02, f5-14, f5-17 e nelle contraddizioni del contratto F5 da chiudere con le decisioni sopra.
