# 2026-10-22-01 — Seconda immagine della 1.0.0 alpha (288052706a…)

## Fusioni della notte (dopo 2026-10-21-10)

| Direttiva | Commit | Esito |
| --- | --- | --- |
| f2-90 + f2-90b | `5d28b71d`, `ce19c3bb` | sfondo letto in una passata prima del primo frame; la frame precalcolata non veniva trovata perché l'identità includeva la mtime FAT e il colore sentinella: ora identità per percorso e dimensione sotto `/system/`, colore del tema |
| f2-89 | `bb8d652f` | il popup «Removable media» della tray non dipingeva la propria faccia |
| f2-88 | `8a181714` | Proprietà schermo decodificava l'anteprima dello sfondo (2,5 MB) sul thread UI e non rispondeva al ping per oltre 5 s; decodifica su worker; il desktop non marca «non risponde» durante attese proprie |
| f6-19 | `d64787e0` | NetSurf riceve `--channel-fd=3` da ogni percorso di lancio (fallback su fd 3); voce «NetSurf — Web browser» nella radice di Programs con icona Tango; entropia: ripristino del jitter secondo la documentazione della libreria, retry limitati, credito degli interrupt (1 bit per 64), fixture che riproduce `rounds=2644 ms=335` |
| f0-55 | `6f121d69` | barra di avanzamento sotto la foto di Ciuki, guidata da 14 fasi di init, ridisegni parziali ≤1.024 px per lock, record di handover |
| f1-93 | `c394506a` | stat percorreva 32.768 cluster di UNIFIED.IMG, readdir/lookup/mkdir leggevano a freddo sotto prenotazione: rilascio e ripartenza attorno a ogni lettura; replay T23 a cache fredda con comandi da 106 ms, tutte le prenotazioni a 0 ms |
| f1-92 | `125fefa2` | causa reale degli errori di I/O: dalla f3-49b una richiesta cancellata o fuori budget terminava un comando ATA emesso e `failure()` lo trattava come guasto (quarantena DMA → PIO → volume in sola lettura); ora un comando emesso termina solo alla scadenza del dispositivo, fronte anticipato gestito, un ritentativo PIO prima della sola lettura |
| f5-15d | `2031739d` | due test del flusso installer rossi per la dimensione di `ciuki_w32_file_info` nel fixture host |

Perdita e recupero: il worktree f6-19 è stato cancellato dal lead prima del
commit (comando concatenato); l'agente ha riapplicato le modifiche e la
consegna è stata verificata di nuovo. Regola registrata.

## Evidenza

- Suite host sullo stato `b8ad81b9` (catena `final-alpha.sh`, 03:04–04:12):
  kernel rc 0, FS con tutti i gruppi superati (`test_fairness` superato al
  rerun a macchina scarica: confronto di throughput a 0,1 %), win32 365 OK,
  DOS 7 OK, piatte 1.164 con i soli errori «rebuild the SDK» superati al
  rerun dopo la build (72 OK). Lo stato finale `ce19c3bb` aggiunge f2-90b
  (test desktop/wallpaper verdi nel worktree).
- Build canonica e immagine (04:22–04:27): `build/f0/ciukios.img`
  `94099db86133d61cda74616f35dda0323226f84b2208c57ab0f64a53a2123042`,
  `build/hw/ciukios-hw.img`
  `288052706a83765832590e594682fadb6f83300c4e738f183e957e55364dbaaf`
  (`BOOT.CFG` `safe=0 serial=1`, fork FreeDOS qualificato, kernel unificato
  predefinito).
- Boot QEMU (profilo qemu-t23, `build/lead-scratch/splash-shot.py`):
  splash con barra a ~13 s, desktop con sfondo nel primo frame a ~17 s,
  `[entropy] health=ok`, nessuna quarantena ATA, nessun `source-read` dello
  sfondo. Osservazione: sotto TCG la barra resta vuota nei 2 s prima dello
  handover (follow-up cosmetico f0-55b).

## Da confermare sul T23

Giochi DOS senza errori di I/O e senza `readonly`, finestre senza scatti
(`vfs_hold` sotto 20 ms), NetSurf dal menu entro 3 s e pagina https dopo
`[entropy] health=ok`, Proprietà schermo, popup supporto rimovibile, sfondo
al primo frame, copia da USB. Log: `C:\SYSTEM\LOGS` e seriale.
