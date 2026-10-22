# 2026-10-22-09 — Sesta immagine della 1.0.0 alpha (3423912f…)

## Fusioni dalla prova a4d9454e (2026-10-22-07)

| Direttiva | Esito |
| --- | --- |
| f2-95 (em. 1–2) | la CPU non era presa dal compositore ma dal worker delle scorciatoie (`stat()` di quattro alberi e tre interrogazioni dell'immagine giochi ogni 2 s, cache del disco occupata) e dal worker dei suoni (poll ogni 2 ms di un ultimo frame mai consumato); i 40 frame/s erano movimenti del cursore; regioni di danno con il codice regioni di pixman 0.46.4 (MIT, importato invariato); modello T23: desktop dal 40,6 % al 2,1 % con il lettore aperto, 17,4 % in trascinamento |
| f1-101 | promozione dei thread che alimentano un flusso audio in ritardo (livello 16, budget comune 20 ms/100 ms, ritardo misurato dal mixer); riserva del lettore 500 ms, prefill 1 s; R2/R3 emendati |
| f2-96 | un solo cursore (il software solo con l'hardware sicuramente nascosto); poll EC ogni 2 s per i tasti del volume (`ecpoll=0` spento, `ecpoll=1` 1 Hz); il tempo SMI dimostrabile escluso dai budget dello scheduler e dalla scadenza audio (`[sched] firmware_credit`) |
| f4-37 | decoder: ipotesi dei denormali smentita per lo stereo (vera solo nel mono, corretta); costo sul modello P3 da ~3 % a 1,3–1,7 % (niente FLDCW per campione, ricampionamento esatto senza divisioni, niente copie); lettore ≤15 fps; record `[music] cost … mhz= loop= mem= best_kc=` per spiegare il 41 % misurato sul T23 |
| f2-97 | `docs/credits.md` (38 componenti) verificato da test; scheda Credits dell'About leggibile e scorrevole; licenza di CiukiOS corretta (GPL-2.0, SDK MIT); documentazione aggiornata |
| lead | conflitti f2-96/f1-101 (`cpustat.c`) e f2-95/f2-96 (compositore, risolto dall'agente); golden `tooltip-task` ri-registrato (finestra About ingrandita); test host aggiornati (`a97ca2c2`) |

## Evidenza

- Gate f0-core su QEMU (immagine intermedia, stesse sorgenti): 92 PASS,
  20:24–21:04.
- Suite host sullo stato `26e7b9f8` (21:04–22:35): kernel rc 0, FS rc 0,
  win32 365 OK, DOS 7 OK; suite piatte 1.194 test con 6 rossi di test non
  aggiornati (scansione dei crediti sulla cartella privata ignorata; budget
  del log da 1 MiB nelle fixture del runner), corretti in `a97ca2c2` e
  verificati (15 OK).
- Build canonica e immagine (22:35–22:37): `build/f0/ciukios.img`
  `3d6ec6200b4ad19d4e1618bb86999b02786f2e18cfeb288d3eaea928f5f2f7c5`,
  `build/hw/ciukios-hw.img`
  `3423912f8463a4a807b65f107c1ff9c38fd391efd88f3b4284633177c32eef89`
  (T1 verdi, 2.099 payload, `safe=0 serial=1`).

## Decisione del proprietario (20:30)

Codex di nuovo disponibile: le prossime modifiche vanno a Codex con il solo
`gpt-6.1-sol` (mai `gpt-6-astra`), reasoning scelto dal lead; `AGENTS.md`
aggiornato (`c81c0076`).

## Da leggere sul T23

`[cpu] top` (desktop#30/#31 fuori dai primi cinque), `[desktop] loop
turns= top=`, `[music] cost … mhz= loop= mem= best_kc=` e `[music] level …
boost=`, `[sched] audio_producer`, `pointer=hw`, `[ec] thinkpad io=` ogni
2 s con i tasti del volume, `[net] dhcp state=bound`, `[usb-storage] … max_sectors=128`.
