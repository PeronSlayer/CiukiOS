# Roadmap CiukiOS Legacy v2

## Visione
Costruire un OS semplice, nativo x86 legacy BIOS, capace di eseguire software DOS e pre-NT senza emulazione.

## Fase 0 - Reset e Fondamenta
1. Archiviazione progetto precedente in `OLD/`.
2. Nuova architettura legacy documentata.
3. Nuove regole operative per sviluppo e agenti AI.

## Fase 1 - Boot Legacy Minimo (Floppy-first)
1. Boot sector 16-bit (512B) + loader multi-stage.
2. Init real mode e servizi BIOS essenziali (INT 10h/13h/16h/1Ah).
3. Kernel minimale x86 con shell basilare.
4. Build `floppy` entro 1.44MB.

## Fase 2 - Compatibilita DOS Nativa
1. Loader `.COM/.EXE` nativo.
2. PSP/MCB + gestione memoria convenzionale/UMB/HMA.
3. INT 21h ad alta compatibilita.
4. FAT12/FAT16 per profilo floppy.

## Fase 3 - Runtime Grafico DOS + OpenGEM
1. VGA/VBE nativi.
2. INT 10h esteso e servizi mouse/timer/input robusti.
3. VDI/AES compat layer nativo per OpenGEM.
4. Milestone: desktop OpenGEM stabile su hardware reale.

## Fase 4 - Target DOOM
1. Ottimizzazione path grafico mode 13h/VGA.
2. DPMI/estender compatibility minima per binari DOS complessi.
3. Milestone: DOOM avviabile e giocabile.

## Fase 5 - Target Windows pre-NT
1. Superficie compat DOS richiesta da Windows 3.x/95/98 bootstrap path.
2. DPMI avanzato, gestione interrupt/timer compatibile.
3. Device/path compatibili per setup e avvio progressivo.
4. Milestone incrementali: Win 3.x -> Win95 -> Win98.

## Fase 6 - Build e Release
1. Profilo `floppy`: minimale, diagnostico, portabile.
2. Profilo `full`: runtime esteso, desktop e toolchain completa.
3. Pipeline test regressione legacy hardware + emulatori.

## Criteri di avanzamento
1. Ogni milestone richiede test riproducibili.
2. Nessun merge su `main` senza approvazione esplicita utente.
3. Niente scorciatoie con emulazione software CPU come soluzione finale runtime.
