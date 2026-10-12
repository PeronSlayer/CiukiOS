# Scope: giochi Windows tramite API Win32 compatibile; immagine definitiva 36010476 della giornata del 12 ottobre

- **Data:** 2026-10-13
- **Tipo:** decisione (scope); fusioni finali e consegna hardware
- **Versione/commit:** `main` `4acdc785` (immagine `36010476…`, immagine hardware `fe695509…`)
- **Stato:** immagine consegnata al proprietario per la prova sul T23

## Decisione (proprietario, 2026-10-13)

Lo scope dei giochi non DOS cambia: l'obiettivo è un'**API Win32 compatibile
e funzionante** su CiukiOS, per installare ed eseguire i giochi Windows
dell'epoca senza passaggi aggiuntivi. Sostituisce "giochi nativi compilati dai
sorgenti" (F5) e "giochi Windows tramite Wine" (F8): F5 diventa la personalità
Win32, F8 i port nativi opzionali. README, roadmap e AGENTS.md aggiornati; il
contratto tecnico della personalità Win32 si scrive e si revisiona prima del
codice, come per gli altri passi.

## Fusioni della serata (12 ottobre, dopo la 2026-10-12-05)

f2-38 (sequenza di spegnimento: chiude finestre e VM, ritenta EBUSY),
f1-24 (write-combining MTRR sul framebuffer, misurazione dei present),
f2-39 (record di latenza di avvio, letture posizionali dello store icone,
letture ELF più grandi), f3-09 (DOOM sul T23: bit DR6 di slot non allocati
trattati come watchpoint; record diagnostici in modalità L), f1-23 (modi
video: bootinfo v2 con tabella VBE, chiamata 74, cambio modo tramite VM
firmware), f2-37 (proprietà schermo: risoluzione, colori, scheda Adapter),
f4-06 (suoni di sistema: pack Kenney Interface Sounds CC0, eventi, scheda
Sounds con test hardware), f6-00 (contratto di rete, bozza da rivedere),
f5-01 (Tiny C Compiler sul target, CiukIDE, guida dello sviluppatore),
f6-01 (driver NIC Intel 8255x, RTL8139, NE2000 PCI), f2-41 (Control Panel a
cartella, Device Manager con chiamata 76, Driver, Sistema), f2-40
(CiukPaint). Correzione del lead: il loader F0 sforava di 12 byte la sua
immagine fissa; accorciato il messaggio di rifiuto della CPU (modello del
loader e baseline hardware allineati); riferimento API rigenerato.

## Verifiche

Test host sull'albero `4acdc785`: desktop 66 OK, gate 10 OK, sviluppo nativo
12 OK (7 saltati per il sandbox), dispositivi 2 OK, payload 7 OK, test host del
kernel OK. Immagine `36010476…` (albero pulito): `f0-smoke` 4 PASS.
Immagine hardware `build/hw/ciukios-hw.img` SHA-256
`fe6955097dafa273eaa660fc1825733b4e2b4569bb915c8ee83cf1bdcdba32aa`.

Prove sul T23 del 12 ottobre (immagini `569b3fca…` e `1f357494…`): avvio
normale, desktop, Files, spegnimento ACPI con la nuova sequenza, MTRR WC
applicato al SuperSavage (`[display] mtrr wc base=e8000000 size=4194304
result=ok`); DOOM si fermava a `I_StartupTimer` (corretto in f3-09, da
riprovare).

## Metodo

Le sessioni Codex in parallelo saturavano la memoria del posto di lavoro
(14 GiB, sei sessioni uccise da systemd-oomd alle 21:26): da ora ogni
sessione gira in uno scope con tetto di memoria, al massimo due o tre
contemporanee, con guardia sulla memoria disponibile
(`build/lead-scratch/codex-queue.sh`).
