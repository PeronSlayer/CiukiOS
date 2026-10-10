# Contratto F4 approvato: audio, media, volumi condivisi, ottico e NTFS in sola lettura

- **Data:** 2026-10-11
- **Tipo:** decisione (contratto di progetto)
- **Versione/commit:** `main` dopo la fusione di `wt/f4-contract`
- **Stato:** completato

## Contesto

La transizione delle fondamenta impone un contratto scritto e revisionato in
modo incrociato prima del codice di ogni passo. F4 (audio e media) non ne
aveva uno; la bozza è stata scritta da Codex (direttiva f4-00) con ricerca
sulle fonti primarie (AC'97 2.3, datasheet ICH3, QEMU AC97, es1968 Linux,
guida Sound Blaster, YMF262, riferimento MS-DOS 3.3 e FreeDOS, SFF-8020i,
ECMA-119 e Joliet, NTFS3) e con il confronto con il codice conservato
(`session_audio.c`, `guest_peripherals.c`, `session_opl.cpp`, CIUKIDOS).

## Decisione

`docs/design/f4-audio-media-contract.md` è il contratto F4. Punti fissati
dal lead sulle domande aperte della bozza:

1. Backend audio: prima Intel ICH AC'97 con il gemello QEMU AC97 come
   riferimento di protocollo; poi ESS Maestro-2E, qualificato solo sull'E500.
2. I record di controllo del dispositivo audio e i loro numeri vengono
   fissati dalla prima direttiva F4 con emendamenti a `execution-abi.md` e
   `posix-subset.md`, prima del codice.
3. La matrice di condivisione e blocco (modalità DOS, compatibilità,
   intervalli) è adottata e va riportata in `vfs-storage-contract.md` dalla
   direttiva del bridge, prima del suo codice.
4. Budget di latenza 60 ms, quanto da 256 frame e otto periodi pubblicati
   sono obiettivi provvisori: le misure T4 su T23 ed E500 li confermano o
   li rivedono con una voce di diario.
5. Varianti da qualificare sui PC assemblati: controller AC'97 82801AA,
   82801BA e 82801DB.
6. Accelerazione 2D S3/Mach64 rinviata e disattivata.
7. NTFS in sola lettura con un driver proprio `ntfs_ro` (GPL-2.0-only),
   senza importare codice NTFS3.
8. API audio nativa, mixer, backend AC'97, montaggio ATAPI/ISO 9660 e NTFS
   in sola lettura non dipendono da F3 e possono procedere in parallelo;
   percorso audio delle VM DOS e bridge DOS→VFS seguono f3-05 e f3-00.

## Verifiche

Nessuna verifica di runtime: il contratto definisce le prove di accettazione
(T0–T4) che le direttive F4 dovranno soddisfare.
