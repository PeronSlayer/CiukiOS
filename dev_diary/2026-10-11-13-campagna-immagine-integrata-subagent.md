# Campagna QEMU dell'immagine integrata fcd96d6e; lavoro parallelo con subagent Claude verso la pre-alpha 0.9.0

- **Data:** 2026-10-11
- **Tipo:** modifica (correzioni) e analisi; decisione di metodo
- **Versione/commit:** `9f17715` (immagine `fcd96d6e…`); correzioni `a9c0b31`, `e222a5f`, `5342d58`, `9f17715`, `1b9f623`
- **Stato:** immagine qualificata su QEMU; verifiche dei nuovi lotti in corso

## Contesto

Dopo la fusione di f3-03 (voce 2026-10-11-12) la qualifica dell'immagine
integrata ha fatto emergere quattro difetti che bloccavano ogni suite del
runner o la sonda F0 `preempt`. Il proprietario ha inoltre deciso di
accelerare: il credito di Codex resta esaurito, quello di Claude è stato
aumentato, e il lavoro sulle direttive aperte passa a subagent Claude in
parallelo, con il modello scelto per compito, verso una pre-alpha 0.9.0
utilizzabile e presentabile.

## Cosa cambia

1. Il test host dell'allocatore di pile asseriva ancora 16 KiB dopo il
   passaggio a 32 KiB: corretto, insieme al documento ABI (`a9c0b31`).
2. Il thread di riempimento audio interrogava il dispositivo ogni
   millisecondo a priorità `P_DEVICE` (lo schema di f1-18) e affamava i
   task ring-3: `preempt` falliva con zero commutazioni. Il thread si blocca
   fra un servizio e l'altro con scadenze di sicurezza di 1/4/50 ms
   (`e222a5f`). I record delle VM sono intestati alla sonda proprietaria
   (`dosvm_config.probe`).
3. Il generatore di fixture NTFS lasciava byte stantii dopo i nomi nei
   FILE_NAME: il test di determinismo rifiutava le suite. Il normalizzatore
   azzera quel padding (`5342d58`).
4. La sonda `fat-read` calcolava l'hash di ogni file del volume, compreso il
   `DOSBOOT.IMG` da 128 MiB: `fat12-read` superava i 5 minuti. I file oltre
   8 MiB sono registrati come `case=large`; T1 ne verifica l'hash
   staticamente (`9f17715`).
5. Il vettore EMS è F100:0000: la rilevazione LIM legge il nome all'offset
   000Ah del segmento del gestore, e F000:1000 collideva con la
   continuazione del callback INT 33h di f3-01 (`1b9f623`).

Rilevazione di metodo: build e harness in parallelo alle suite QEMU fanno
sforare le scadenze host dei casi TCG; le campagne girano a macchina quieta.

## Verifiche

Immagine `fcd96d6e…`: `f3-vm` 195 PASS (prefisso F0–F2, poi `vm-boot` e
`vm-memory` sui profili t23, e500 e min128), `f0-smoke` 2/2, `f2-all`
189 PASS; i tre casi `uart-absent-*` restano casi operatore. Prima immagine
integrata con le VM DOS funzionanti.

Consegne dei subagent (committate sui rispettivi branch, QEMU da eseguire):
f3-01 (causa del fallimento della fase tastiera: il payload mascherava IRQ1
con OCW1 FEh; corrette la continuazione del callback mouse e la
sostituzione di INT 33h da parte di CIUKIDOS), f3-02 (VGA intercettata,
`/bin/dosbox`, sonde video e finestra), f2-31 (toolkit e Files completi),
f2-32 (CiukNote), f2-33 (Task Manager con `proc_list` 70 e
`proc_terminate` 71), f4-04 (montaggi NTFS e slot cache). Il Task Manager
è già riportato sul toolkit riscritto; CiukNote in corso.

f3-01 fusa su `main` nel pomeriggio: `vm-devices` PASS su qemu-t23, e500 e
min128 (`case=input keyboard=1 focus_release=1 callbacks=2 keys=3`,
`case=locality malformed_io=1`). Oltre alla maschera OCW1, il payload
dormiva con `hlt` dopo che entrambi gli IRQ1 erano già stati consegnati al
confine dopo STI: i cicli di attesa ora verificano la condizione a
interruzioni spente e dormono con `sti; hlt`.

Nel tardo pomeriggio sono state fuse anche f3-02 (`36e9c12`: VGA
intercettata su A0000h–BFFFFh, risposte INT 10h/VBE, font, `/bin/dosbox`
con la syscall 72, sonde `vm-video` e `vm-window`) e f4-04 (`f524207`):
la campagna 7 sull'immagine integrata (`f3-vm` a 14 casi, `f0-smoke`,
`f2-all`, `f4-media`) è in corso; l'esito va in una voce successiva.

## Decisioni

- Le direttive aperte si realizzano con subagent Claude: Opus per kernel,
  monitor e interfacce grandi; Sonnet per app e colla ben specificate. I
  subagent non eseguono QEMU né costruiscono immagini: il lead fonde,
  qualifica e scrive il diario.
- Le fusioni avvengono a lotti per immagine (DOS: f3-01 e f3-02; desktop:
  f2-31, f2-32, f2-33; f4-04), perché ogni immagine costa circa tre ore di
  QEMU.
- Numerazione syscall: 70 e 71 al Task Manager (f2-33), 72 alla VM DOS
  (f3-02).
- Obiettivo dichiarato: pre-alpha 0.9.0 con desktop (Files, CiukNote, Task
  Manager), finestra DOS con COMMAND e un programma VGA, audio; DPMI e DOOM
  se i tempi lo consentono. La pubblicazione resta sospesa da
  `config/release-policy.json` fino a decisione del proprietario.
