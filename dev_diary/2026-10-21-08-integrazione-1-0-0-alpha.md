# 2026-10-21-08 — Integrazione della 1.0.0 alpha

## Contesto

Mandato del proprietario (2026-10-21, 19:00–20:30): completare il programma
in corso e produrre l'immagine della 1.0 Alpha nella stessa serata, con le
prove tutte sull'hardware (T23); Setup da CD rinviato (2026-10-21-07). Dopo
l'esaurimento dei crediti Codex e Antigravity, l'implementazione è stata
svolta da agenti Claude (Opus per kernel e architettura, Sonnet per il resto)
in worktree separati, con revisione incrociata e fusione da parte del lead.

## Fusioni in `main` (in ordine)

| Direttiva | Commit | Contenuto |
| --- | --- | --- |
| f0-54 | `f162192b` | budget byte del loader: matcher condiviso, tabella program header su buffer E820; 1.303 byte liberi |
| f1-87 (em. 1–4) | `c19230eb` | correzioni dal trial T23: prenotazioni volume etichettate, priorità refill audio, VM in finestra disaccoppiata dal presenter, token `usbscan=0` |
| f1-90 (em. 1) | `3c50bb79` | attribuzione del budget CPU dispositivi per task (fair share max-min), classe refill audio con budget proprio di 2 ms e wake diretto dall'interrupt; contratto R1/R2 emendato |
| f5-ini | `a17e29f8` | kernel32 `WritePrivateProfileStringA`: chiavi duplicate, cancellazioni con NULL, fine riga conservati |
| f6-18 | `311d33bc` | casi HTTPS del runner per NetSurf: selettore `f6:browser` (grammatica URL completa nel loader), sonda kernel, profilo di test con ancore fixture, `https_fixture.py` |
| f5-15a2 (em. 2–12) | `a72352fd` | installer NSIS/CAB/autoestraenti: advpack/setupapi/cabinet, ReadFile a lunghezza zero, percorsi case-insensitive, sonda `w32-installer-nsis` |
| f5-15b | `01fa33ec` | accettazione degli installer originali: dieci installer caricati, workload importati con ricevute SHA-1, censimento |
| f3-56 (em. 1) + f3-57 (em. 1) | `d0e05169` | DOS unificato: binding DFV2 nativo, driver `CIUKIDF.SYS`, patch 0002 del fork FreeDOS 2043 (ciclo di vita, siti redirector, FCB/NLS, correzione GS), backend VFS reale per C:/D:/E:, errori critici con frame INT 24h, attivazione rifiutata → VM fermata con record, installazione nell'immagine sotto `kernel=unified` subordinata a `fork-qualified.json` |
| f5-15c | `90c9e903` | tier installer QEMU: dieci sonde `f5:win32-<alias>` registrate (kernel, tabella loader, modello runner), driver UI guest, manifesti e calibrazione, CD installer nel runner, `_vsnprintf`/`_vsnwprintf` |
| f1-89 (em. 1–2) | (in corso) | pubblicazione cache riservata e transazioni FAT: nessuna barriera per blocco da 8 KiB, turno scrittore prima della risoluzione del percorso, transazioni a pagine singole, lettori in volo bloccano la pubblicazione con metrica di attesa, lookup guardati senza letture disco |

Correzioni del lead su `main`: marcatori di conflitto committati per errore
nella fusione di f5-15a2, risolti in `a72352fd` (51 test loader/runner);
link di `desktop_test` rotto dalla sonda browser di f6-18, riparato in
`d4f56c71` con stub host (303.518 controlli).

Loader: 887 byte liberi nell'estensione fissa dopo le registrazioni di
`usbscan=0`, `f6:browser` e delle dieci sonde titolo.

## Versione

`config/version.json` passa a `1.0.0 alpha` (`VERSION`, `README.md`,
intestazione `CHANGELOG.md`, `docs/release-notes-1.0-alpha.md`).

## Evidenza e immagine

Catena finale `build/lead-scratch/final-alpha.sh`: suite host (kernel, fs,
piatte, win32, dos), qualificazione del fork (`run_unified_file.sh`), build
canonica sotto scope da 3 GiB, immagine hardware con `BOOT.CFG`
`safe=0 serial=1` e SHA-256. Risultati e SHA-256: da aggiungere al termine.

## Rinviato dopo la 1.0.0 alpha

f3-58, f3-59 (matrice di condivisione, celle di classificazione unità, test
INSTALL=, record di traccia; shell, libreria, ritiro), f1-91 (clock VM audio
su task di classe audio), f5-17b (driver del tier titoli), calibrazione QEMU
del tier installer per titolo, segnalazione a monte del difetto GS di FreeDOS
2043, follow-up 5–10 della revisione f1-89 (abort dopo passo durevole
parziale per primo), Setup da CD (f9).
