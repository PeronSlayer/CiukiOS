Sei su CiukiOS. Lavora SOLO sul track video (non Phase 2 DOS core, già chiusa su main).

Obiettivo:
Portare avanti il sub-roadmap video oltre la baseline 1024x768, implementando una policy di backbuffer dinamico/più grande e compatibilità sopra 1024x768 senza fallback diretto di default.

Contesto attuale da rispettare:
- Loader marker runtime: `GOP: policy1024 available=... selected=... result=PASS/FAIL`
- Gate esistenti: `make test-video-1024`, `make test-video-mode`
- Gate globali da non rompere: `make test-mz-regression`, `make test-phase2`

Task richiesti:
1. Implementa policy di allocazione backbuffer più robusta/dinamica in stage2 video.
2. Migliora selezione/compatibilità modalità oltre 1024x768 mantenendo comportamento deterministico.
3. Aggiungi o aggiorna test non interattivi che validino esplicitamente il nuovo comportamento.
4. Aggiorna roadmap e crea handoff dedicato.

Vincoli:
- Non rompere ABI loader/stage2.
- Mantieni build riproducibile (`make all`).
- Se trovi problemi di capture QEMU, classificali come INFRA con diagnostica chiara.

Checklist finale obbligatoria:
- `make all`
- `make test-video-1024`
- `make test-video-mode`
- `make test-mz-regression`
- `make test-phase2`

Output atteso:
- Commit(s) focalizzati su video
- Roadmap aggiornata
- Handoff: `docs/handoffs/2026-04-17-<video-topic>.md`
