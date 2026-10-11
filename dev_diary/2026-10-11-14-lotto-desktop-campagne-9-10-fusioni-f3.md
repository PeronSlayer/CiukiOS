# Lotto desktop fuso; campagne 9 e 10; fusione testuale di f3-05 e f3-04; agenti in parallelo verso la 0.9.0

- **Data:** 2026-10-11
- **Tipo:** modifica (fusioni e correzioni) e analisi di regressione; decisione di metodo
- **Versione/commit:** `1241de1` (lotto desktop), `daff47e` (pin dei device); immagini `e896ed38…` e `4595cd98…`
- **Stato:** in corso (prossima immagine dopo le fusioni F3/F4)

## Contesto

Dopo la fusione di f3-01, f3-02 e f4-04 (voce 13) la campagna 9 si è fermata
al primo caso F3: `vm-boot-qemu-t23` fissava tre device per VM, ma dalla
fusione di f3-02 l'adattatore VGA è il quarto device autoritativo di ogni VM
(`dosvm_snapshot`). La sonda passava; il pin era vecchio. La catena di suite
si è interrotta lì (`f0-smoke`, `f2-all` e `f4-media` non sono girate).

## Cosa cambia

1. Lotto desktop fuso su `main` (`1241de1`): toolkit libdeskui riscritto,
   Files, CiukNote, Task Manager, protocollo finestre v2 (opcode 15–20),
   chiamate 70 (`proc_list`) e 71 (`proc_terminate`), tre casi
   `files/tasks/ciuknote-qemu-t23` con digest da fissare al primo run.
2. Pin dei device a 4 nei tre casi `vm-boot`; suite scratch rimosse (`daff47e`).
3. Campagna 10 sull'immagine `4595cd98…`, limitata alle suite influenzate
   (suite scratch senza prefisso F0–F2, poi `f2-process`, `f2-app`,
   `f4-media`, `f0-smoke`). Esiti e difetti nel documento di validazione:
   - `vm-memory-qemu-t23`: il payload VMMEMORY falliva al controllo A7
     perché dal modello 8042 di f3-01 una risposta del controller alza IRQ1
     come sull'hardware reale e l'ISR tastiera di CIUKIDOS consumava la
     risposta D0h prima del poll dello stato. La sequenza D1h/D0h del
     payload gira a interruzioni spente. Il dump dello schermo di una VM
     fallita conteneva `\` e `>`, rifiutati dalla grammatica dei record:
     i glifi fuori da `[A-Za-z0-9_:.,/+-]` diventano `.`.
   - `files-qemu-t23`: il desktop riconosce F11, ma Files non raggiunge il
     primo checkpoint e la scadenza di 40 s della sonda scade mentre il
     runner ha consegnato 138 dei 176 lotti di stimoli (QMP costa circa
     0,29 s per lotto). Scadenza dei casi app portata a 180 s; i tre script
     vengono riprodotti su host contro il codice reale di desktop e app
     prima del prossimo run; il runner conserva `screen-fail.ppm` per i casi
     desktop e finestra DOS falliti.
   - Il test host `test_dosvm` modellava ancora tre device: corretto (la
     prerequisito host rifiutava `f2-process`, `f2-app`, `f4-media`).
4. f3-05 (SB16/OPL3/8237) e f3-04 (DPMI) fusi testualmente nel worktree
   `wt/f3-merge` (`9aacea7`, `b5325f0`): 24 conflitti risolti dal lead
   tenendo entrambi i lati e combinando le tabelle delle sonde (loader,
   selettore, modello del runner, ordine di link: vm, devices, video,
   audio-state, dpmi), la catena dei trap (dispatch DPMI in modo protetto
   prima, #PF V86 su A0000h–BFFFFh al modello VGA), `dosvm_low_flags` con
   le regole VGA/ROM/pila reale DPMI, inizializzazione VGA poi ROM DPMI,
   parser dei record con latch VMVIDEO e verdetto generico, harness host con
   le tre parti. Compilazione e test host affidati a un agente dopo la
   campagna.

## Verifiche

Immagine `e896ed38…`: prefisso F0–F2 189 PASS. Immagine `4595cd98…`:
`vm-boot` PASS su t23, e500 e min128; 13 casi desktop esistenti PASS;
`f0-smoke` 2/2. I casi F3 restanti, i casi app e le suite F2/F4 attendono la
prossima immagine.

## Decisioni

- Le campagne usano suite scratch senza prefisso quando il prefisso è già
  passato sulla stessa immagine o la modifica non può influenzarlo; una
  catena di suite non si interrompe al primo fallimento.
- Un lotto di merge va fuso prima della campagna successiva, non dopo:
  un'immagine, una campagna.
- Direttive aperte in parallelo con agenti Claude: integrazione di
  `wt/f3-merge` (compilazione, test host, difetti di integrazione),
  f3-06 (sonda DOS/4GW, DOOM nella VM con accesso VGA dal modo protetto,
  dischi fixture locali mai nell'immagine di release), f4-05 (uscita audio
  delle VM DOS: PCM SB16 e sintesi OPL3 nel mixer AC'97, direttiva da
  scrivere prima del codice), identità di versione 0.9.0 pre-alpha
  (CHANGELOG, README, stringa di versione senza toccare superfici con
  digest fissati), riproduzione su host dei tre script desktop.
- La build dell'immagine dipende ora da Open Watcom 2.0 in `/opt/watcom`
  (client DOS/4GW di f3-04); il pin in `config/toolchain.json` e la
  redistribuzione di `DOS4GW.EXE` restano decisioni del proprietario prima
  di riattivare la pubblicazione.
