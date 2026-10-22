# 2026-10-22-04 — Quarta immagine della 1.0.0 alpha (5a10eef4…)

## Fusioni dalla prova b453bc92 (2026-10-22-03)

| Direttiva | Esito |
| --- | --- |
| f1-95 | Savage: il primo riempimento dopo il reset del motore ripete il byte basso; self-test con innesco, marcatore e copia; attesa `caps=3` |
| f0-56 | log su disco: classi protette, budget delle statistiche, 2,75 MiB per boot |
| f1-97 | «2D acceleration: YES/NO (motivo)» allo splash, nella tray e nel record `[accel] summary` |
| f3-61, b, c, d | il kernel unificato non era mai stato collegato al file system (ordine delle richieste DFV2, mount sul settore 0); ora `DFV2 IFS active`; caso runner `f3:vm-game`; su QEMU DOOM si avvia e legge il WAD attraverso l'IFS, ma in finestra resta «frozen» per il tracciamento a passo singolo degli interrupt sotto TCG (554.800 trap SB in 25 s; f3-35): verdetto rinviato al T23 |
| f1-93b | le prenotazioni lunghe erano prelazione del detentore con la VM a priorità 9; i passi del file system non raggiungono più un punto di prelazione con il disco in mano |
| f1-98 | ereditarietà di priorità per i detentori di prenotazioni e lock; quota del 25 % al desktop mentre gira un gioco (confronto cartella 2.120→88 ms sul modello); R2 emendato |
| f2-88b | Proprietà schermo: resample sul worker, blit per riga; riproduzione QEMU che apre l'applet in 2 s |
| f6-20 | NetSurf: pagine locali esenti dal budget di layout; una bocciatura non termina il browser; motivo di uscita nel log; causa dell'uscita sul T23 = finestra scaricata dal desktop |
| f2-91 | il desktop unisce i movimenti del mouse per le finestre v3 e scarica una finestra solo dopo 5 s di silenzio |
| f1-96 | USB: la divisione adattiva della coda fissava le chiavette a una pagina per comando; trasferimenti da 64 KiB (modello: 85 KB/s → 919 KB/s) |
| f1-94 (em. 1) | attribuzione SMI, `LEGACY_USB_EN` azzerato dopo il handoff (R1 emendato), poll EC a 1 Hz e opzione `ecpoll=0`, record di coincidenza; ipotesi: poll EC intercettato in SMI |
| f6-21, b | raccolta jitter in task idle a turni da 1 ms (gate `preempt`); PDE kernel con bit Accessed preimpostato (gate `isolation`) |
| f5-15d | fixture host installer: dimensione `ciuki_w32_file_info` |

## Evidenza

- Build canonica (12:51–12:53, commit `7226eb9b`): `build/f0/ciukios.img`
  `0cc67ec7fb903be78ea8778f2777b70570860120fe140e28eb875f6fc1c543c6`;
  `build/hw/ciukios-hw.img`
  `5a10eef4a82101c6b0e1b12e336cb0edfbe7d70695ee6cfe688123992f43cfd2`
  (`safe=0 serial=1`, fork FreeDOS qualificato, kernel unificato predefinito).
- Gate f0-core su QEMU su questa immagine: 24 casi PASS compresi `isolation`
  e `preempt`, interrotto dal lead prima della fine su richiesta del
  proprietario (13:04, riavvio di VS Code); nessun FAIL.
- Suite host: verdi nei worktree di ogni consegna sul rispettivo stato; la
  corsa completa sullo stato finale non è stata eseguita (interrotta con la
  catena). Da rieseguire prima della prossima fusione.

## Da leggere sul T23

`[accel] summary accelerated=`, splash «2D acceleration», `[kernel] smi` e
`smi_coincide` (cadenza dei blackout con il poll EC a 1 Hz; secondo boot con
`ecpoll=0` se invariata), `[sched] inherit`/`desktop_share`, `vfs_hold`,
`[usb-storage] bind … max_sectors=128`, `DFV2 IFS active`, `[netsurf] exit
reason=`, `[user N] [control] display …`, `[bootlog] budget_dropped`.
