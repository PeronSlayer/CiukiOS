# Lavoro in simbiosi tra Claude Code e Codex

- **Data:** 2026-10-09
- **Tipo:** decisione (processo)
- **Versione/commit:** nessuno
- **Stato:** completato; ruoli precisati il 10 ottobre ([2026-10-10-01](2026-10-10-01-direttive-f1-codex.md))

## Contesto

Installato il server MCP `claude-codex-bridge` (0.3.1) per comandare Codex
direttamente da Claude Code e far lavorare i due agenti insieme.

## Cosa è stato verificato

- Codex CLI 0.159.3.
- Modelli disponibili (`~/.codex/models_cache.json`):
  - `gpt-6.1-sol`: modello di lavoro attuale; effort da `low` a `ultra`;
  - `gpt-6-astra`: il più capace; effort da `low` a `ultra`;
  - `gpt-6-luna`: veloce ed economico; effort da `low` a `max`;
  - generazioni precedenti: `gpt-6-sol`, `gpt-5.6-*`.
- Il bridge MCP lancia `codex exec` ma:
  - non può impostare l'effort e usa quello di default della configurazione;
  - ha una lista modelli ferma ai `gpt-5.x-codex`;
  - va in timeout dopo 10 minuti;
  - riprende sempre lo stesso thread Codex senza riapplicare la sandbox.
- La CLI diretta rispetta modello ed effort (verificato nel log di sessione
  con `gpt-6-luna`, effort `low`, sola lettura).

## Cosa cambia

Le regole sono in `AGENTS.md`, sezione *Working method*, e valgono per
entrambi gli agenti:
- **Ruoli:** Claude guida (piano, integrazione, commit, push, validazione
  finale); Codex fa ricerca, revisioni indipendenti e implementazioni
  delimitate. I disaccordi vanno riportati al proprietario.
- **Modello ed effort:** Luna per ricerca e controlli semplici, Sol per le
  revisioni e le implementazioni, Astra per i problemi più difficili; `ultra`
  solo su richiesta del proprietario.
- **Scritture:** un solo agente scrive nell'albero principale; le
  implementazioni di Codex avvengono in un git worktree separato, che chi
  guida rivede e integra.
- **Revisione incrociata:** prima delle decisioni di architettura e prima dei
  commit importanti.

## Primo lavoro comune

Revisione indipendente della proposta di architettura
([2026-10-09-02](2026-10-09-02-revisione-architettura.md)) affidata a
`gpt-6-astra` con effort `xhigh`, in sola lettura.
