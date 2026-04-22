# CiukiOS (Legacy Reset)

CiukiOS riparte da zero con un obiettivo chiaro: sistema operativo x86 legacy-first, senza dipendenze UEFI, orientato all'esecuzione nativa software DOS e pre-NT.

## Obiettivi principali
1. Boot ed esecuzione su hardware retro x86 reale (Intel/AMD) in modalita legacy BIOS.
2. Esecuzione nativa applicativi DOS (nessun layer di emulazione): OpenGEM, DOOM e target Windows fino a 98 (pre-NT).
3. Due profili build:
   - `floppy`: build minimale entro 1.44MB, avviabile su PC legacy.
   - `full`: build completa con runtime esteso e stack desktop.

## Stato repository
1. Codice storico archiviato in `OLD/archive-2026-04-22/`.
2. Documentazione nuova in `docs/`.
3. File storici mantenuti: `CHANGELOG.md` e handoff storici.

## Documenti chiave
1. `docs/architecture-legacy-x86-v1.md`
2. `Roadmap.md`
3. `docs/diario-bordo-v2.md`
4. `docs/ai-agent-directives.md`

## Comandi base
```bash
make help
make build-floppy
make build-full
```

Nota: le build attuali sono scaffold iniziale del reset (artefatti base), non ancora immagini bootabili complete.
