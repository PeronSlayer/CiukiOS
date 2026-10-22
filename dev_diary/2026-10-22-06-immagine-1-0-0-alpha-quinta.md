# 2026-10-22-06 — Quinta immagine della 1.0.0 alpha (a4d9454e…)

## Fusioni dalla prova 5a10eef4 (2026-10-22-05)

| Direttiva | Esito |
| --- | --- |
| f1-100 | causa dei blackout: la linea evento dell'EC (GPI[12]) è instradata su SMI# dal firmware (`gpe1_en=1000`, `gpi_rout=01000000`), due SMI da 2,1 ms per ogni lettura, stato ripulito in SMM; nessun I/O periodico verso l'EC per default (`ecpoll=1` per diagnosi), una lettura al boot, aggiornamento del mirror dei tasti solo aprendo il popup del volume (`MIXER_REFRESH_MASTER`), transazioni contate (`[ec] thinkpad io= transactions=`) |
| f1-96b | la chiavetta spariva per uno stato TD stantio nel ciclo del ring di f1-96 dopo lo svuotamento tra due risvegli (riprodotto sull'host); workaround Linux `uhci_advance` come difesa, ritentativo della stessa catena, scala 128→8→1 prima della quarantena, reset BOT 5 s, re-enumerazione al reinserimento |
| f2-92 | il desktop disegna con il motore Savage: prontezza tardiva gestita (cursore e memoria fuori schermo), attese a fette da 1 ms, `CIUKI_ACCEL_WRITE`/`CIUKI_ACCEL_OFFSCREEN` con self-test al primo uso, cache fuori schermo delle superfici invariate, indicatore «2D engine: OK (drawing: software)» finché non usato, poi «2D acceleration: YES» (`summary … used=`), record `[desktop] accel …` |
| f6-22 | il driver e100 scartava i frame IPv4/ARP/broadcast (bit di stato 5 e 1): nessun DHCP; maschera corretta; record `[net] dhcp/stack/resolver`; righe NetSurf intere (stderr bufferizzato per riga), stato di rete nelle pagine, `NETSURF.LOG` |
| f2-93 | `test_memory_footprint` non riproducibile (7 corse verdi) |
| lead | riferimento API rigenerato (`a03178a2`); test host aggiornati dopo f1-94/f0-56 (`ce388116`) |

## Evidenza

- Gate f0-core su QEMU sull'immagine intermedia `cad2465c…` (stesse sorgenti
  del kernel): 92 casi PASS, compresi `isolation` e `preempt`, 15:23–16:03.
- Suite host sullo stato finale (16:03–17:33): kernel rc 0, FS rc 0, win32
  365 OK, DOS 7 OK; suite piatte 1.171 con due errori di test (include
  mancante nel controller dei fault di segnale dopo f1-94; budget del log
  a 9 MiB dopo f0-56), corretti in `ce388116` e verificati singolarmente.
- Build canonica e immagine (17:33–17:35, commit `a03178a2`, albero con
  `ce388116` ai soli test): `build/f0/ciukios.img`
  `6f930844afcd871a834e76d4cb1b230449f0e1e088f573dad39c059cf8b499b6`,
  `build/hw/ciukios-hw.img`
  `a4d9454ede6bc0504ec406adb02fd900453e927e60383f53445c6da729da23c0`
  (T1 verdi, 2.099 payload; `safe=0 serial=1`).

## Da leggere sul T23

`[kernel] smi` solo nelle finestre con `[ec] … transactions>0` (altrimenti
nessun blackout), `[accel] summary … used=1` e `[desktop] accel copies>0`,
`[net] dhcp state=bound ip=…`, NetSurf che naviga e `NETSURF.LOG`,
`[usb-storage] bind … max_sectors=128` senza `stop=packet`, `DFV2 IFS active`
con un gioco DOS, `[sched] desktop_share` durante il gioco.
