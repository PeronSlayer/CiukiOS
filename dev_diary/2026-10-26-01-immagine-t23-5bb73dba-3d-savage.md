# Immagine T23 5bb73dba…: 3D hardware Savage dietro sonda, Quake III Arena demo, chiusura sempre completa

- **Data:** 2026-10-26
- **Tipo:** modifica, integrazione
- **Versione/commit:** 1.0.1 alpha, main `58727af5f` (+ `be7852468`, solo hash delle DLL Wine); immagine T23 `5bb73dba2b867aafac412b51e911f2dea841a1f954a457f00bf5e7f04c340781`, `ciukios.img` `1709672d…`, ISO di Setup `081378be…`
- **Stato:** completato (prova T23 in [2026-10-26-03](2026-10-26-03-prova-t23-5bb73dba.md))

## Contesto

Dopo la voce [2026-10-25-03](2026-10-25-03-versione-1-0-1-alpha.md) il
proprietario aveva sospeso le immagini fino al 3D funzionante; il 26 ottobre
ha chiesto un'immagine T23 appena integrato f7-11 (Savage a dimensione piena
e stati di Quake III), con le suite in parallelo.

## Cosa cambia

- **Modello universale dei driver 3D (f7-savage).** Servizio `render3d`,
  validazione per famiglia, descrittori di famiglia; l'S3 Savage è il primo
  backend, abilitato solo da una sonda di avvio qualificata.
- **Savage.** Rasterizzazione a funzioni fisse dietro la sonda (f7-03);
  backend hardware di `opengl32` sopra `render3d` con ricaduta software per
  singolo disegno (f7-09); snapshot delle capacità, trasporto `W32_RENDER3D`,
  presentazione con lease a schermo intero (f7-10); ammissione a dimensione
  piena con readback a blocchi e ripristino indipendente, inviluppo di
  precisione di campionamento misurato all'avvio in 23 fasi, blending,
  alpha test, texture 256×256, coordinate REPEAT grandi, divisione dei batch
  al limite di lavoro qualificato, ricaduta dell'intero contesto,
  censimento GL opzionale (f7-11). Senza sonda qualificata il percorso
  software resta identico.
- **Quake III Arena demo.** Gira sul sottosistema Win32 con OpenGL software
  (menu, demo, schermo intero, gamma); OpenGL software con formati
  D16/D24/S8 e span specializzati senza divisioni a 64 bit; avvio corretto
  con i thread Win32 sospesi parcheggiati senza bloccarsi sulla coda della
  trap; il desktop sopravvive alla morte di un client; tastiera senza giro
  delle impostazioni dentro gli eventi di input; installer con pagine e
  posizioni corrette.
- **Chiusura.** Chiusura e spegnimento Win32 che terminano sempre: Termina
  operazione, attese limitate, spegnimento forzato con barriera sui
  dispositivi e reset PCI, slot di emergenza.
- **Utenti (G32b).** `users.lock` recuperabile, password su salvaschermo,
  DPMS e standby, `GetUserName`, HKCU e cartelle della shell per profilo.
- **Altro.** Estensioni degli indirizzi utente in un albero AVL con cache
  dell'ultimo accesso; pagina condivisa Win32 scritta una volta per tick;
  Warcraft II in VBE nativo a schermo intero; latenza della finestra DOS
  (CLI non dimostrabile rifiutata subito, CLI/STI snelli in modalità
  protetta, porte OPL3 snelle).

## Verifiche

`make sdk`, `make build-full` (T1: 2229 payload riletti con dimensione e
SHA-256 coincidenti), smoke QEMU fino al desktop: tutti verdi. Immagine
hardware con `safe=0 serial=1 3d=probe`.
