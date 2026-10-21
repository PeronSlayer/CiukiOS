# Diario di bordo di CiukiOS

Direttiva del proprietario, 9 ottobre 2026: da ora in poi ogni modifica,
decisione o cambiamento **essenziale** del progetto ha una propria voce in
questa cartella, **un file nuovo per ogni cambiamento**. Le voci esistenti non
si riscrivono: se una decisione cambia, si aggiunge una nuova voce che rimanda
alla precedente.

Questa cartella è il diario canonico. I registri precedenti restano come
storia: [`CHANGELOG.md`](../CHANGELOG.md) e, dentro
[`legacy/CiukiOS-docs-legacy-2026-10-09.zip`](../legacy/), il vecchio
`docs/diario-bordo-v2.md`, `docs/history/` e `docs/devlog/`. Tutti i percorsi
`docs/...` citati nelle voci retrospettive si trovano in quello zip.

## Che cosa è "essenziale"

- decisioni di architettura o di direzione del progetto;
- modifiche che cambiano il comportamento del sistema, un'interfaccia (ABI),
  il layout di memoria, il formato del disco o il processo di build/release;
- release e tag pubblicati;
- analisi e report su hardware reale che orientano il lavoro successivo;
- regressioni importanti e la loro causa.

Le correzioni minori (refusi, piccoli ritocchi) non richiedono una voce
propria: confluiscono nella voce del cambiamento a cui appartengono.

## Che cosa non va nel diario

Il diario registra fatti (cose fatte, verifiche, misure) e decisioni con la
loro motivazione tecnica. Non contiene considerazioni interne, cronaca delle
conversazioni, logistica personale (acquisti, disponibilità di cavi o
adattatori, programmi della giornata), configurazioni personali né dettagli
del materiale privato. Le procedure operative stanno nella documentazione
(`docs/`), non nel diario. Togliere da una voce esistente dettagli di questo
tipo è ammesso (fatto il 10 ottobre 2026 su tutte le voci); cambiarne le
decisioni no.

## Nome dei file

`AAAA-MM-GG-NN-titolo-breve.md`, dove `NN` è il numero progressivo nella
giornata (`01`, `02`, …). L'ordine alfabetico coincide con l'ordine
cronologico. Le voci retrospettive usano la data d'inizio del periodo che
descrivono.

## Modello di voce

```markdown
# Titolo

- **Data:** AAAA-MM-GG
- **Tipo:** decisione | modifica | analisi | release | retrospettiva
- **Versione/commit:** 0.8.x / hash (se esiste)
- **Stato:** proposta | in corso | completato | superato da <voce>

## Contesto
Perché serviva il cambiamento.

## Cosa cambia
Il cambiamento in sé, con i file o i componenti toccati.

## Verifica
Che cosa è stato provato, su quale immagine e su quale macchina
(QEMU, T23, E500). Ciò che non è stato provato va detto esplicitamente.

## Riferimenti
Documenti di design, validazione, fonti esterne.
```

## Indice

| Voce | Periodo | Tipo |
| --- | --- | --- |
| [Prima generazione UEFI/x86_64](2026-04-15-01-prima-generazione-uefi-x86-64.md) | 15–21 apr | retrospettiva |
| [Reset verso BIOS legacy x86](2026-04-22-01-reset-bios-legacy.md) | 22 apr – 3 mag | retrospettiva |
| [DOOM giocabile e live/install CD](2026-05-04-01-doom-giocabile-e-cd.md) | 4 – 17 mag | retrospettiva |
| [SHELL.COM e kernel CIUKIDOS separato](2026-05-18-01-shell-com-e-ciukidos.md) | 18 mag – 1 set | retrospettiva |
| [Desktop nativo e monitor V86](2026-09-26-01-desktop-nativo-e-monitor-v86.md) | 26 – 29 set | retrospettiva |
| [0.8.0 – 0.8.3: finestre DOS, app, rete](2026-10-01-01-release-0-8-0-0-8-3.md) | 1 – 5 ott | retrospettiva |
| [Hardware reale T23/E500](2026-10-05-01-hardware-reale-t23-e500.md) | 5 – 9 ott | retrospettiva |
| [Report log fisici diskseq 39](2026-10-09-01-report-log-fisici-diskseq39.md) | 9 ott | analisi |
| [Revisione dell'architettura](2026-10-09-02-revisione-architettura.md) | 9 ott | decisione (proposta) |
| [Pulizia di cartella locale e repository](2026-10-09-03-pulizia-repository.md) | 9 ott | modifica |
| [Lavoro in simbiosi Claude–Codex](2026-10-09-04-lavoro-in-simbiosi-claude-codex.md) | 9 ott | decisione (processo) |
| [Revisione di Codex sull'architettura](2026-10-09-05-revisione-codex-architettura.md) | 9 ott | analisi |
| [Decisione: fondamenta a 32 bit](2026-10-09-06-decisione-fondamenta-32bit.md) | 9 ott | decisione |
| [I sette contratti prima di F0](2026-10-09-07-contratti-f0.md) | 9 ott | decisione (design) |
| [Gemelli virtuali T23 ed E500](2026-10-09-08-gemelli-t23-e500.md) | 9 ott | decisione (test) |
| [F0: kernel, loader e runner su QEMU](2026-10-09-09-f0-kernel-loader-runner.md) | 9 ott | modifica |
| [Scope: retrogaming, API POSIX, rete](2026-10-09-10-scope-retrogaming-posix-rete.md) | 9 ott | decisione (scope) |
| [Direttive F1 a Codex: servizi del kernel, FAT/VFS, runner](2026-10-10-01-direttive-f1-codex.md) | 10 ott | decisione (processo) |
| [Revisione del commit 2003adb e direttiva f0-01](2026-10-10-02-revisione-2003adb-e-direttiva-f0-01.md) | 10 ott | analisi e modifica |
| [F1 e F2 si integrano su main; prova hardware unica](2026-10-10-03-integrazione-f1-f2-su-main.md) | 10 ott | decisione (processo) |
| [Correzioni F0 integrate; primi moduli F1 su main](2026-10-10-04-f0-corretto-e-primi-moduli-f1.md) | 10 ott | modifica |
| [Contratto F2: sottoinsieme POSIX, ABI nativa v1, accettazione](2026-10-10-05-contratto-f2-posix.md) | 10 ott | decisione (design) |
| [SDK F2: header ABI, port di newlib, crt0, pthread, ciuki-cc](2026-10-11-01-sdk-newlib-e-abi.md) | 11 ott | modifica |
| [Integrazione notturna: dispatch F1, driver, processi F2, segnali, BIOS VM, Lua](2026-10-11-02-integrazione-f1-f2-notte.md) | 11 ott | modifica |
| [Driver avviati al boot, storage FAT montato, oggetti del desktop, suite F1 allineate](2026-10-11-03-driver-al-boot-storage-desktop.md) | 11 ott | modifica |
| [Prime sonde F1 superate nel guest; file POSIX, desktop e correzioni](2026-10-11-04-prime-sonde-f1-nel-guest.md) | 11 ott | modifica |
| [Obiettivo hardware: anche i PC desktop assemblati, con un'unica immagine](2026-10-11-05-pc-generici.md) | 11 ott | decisione (scope hardware) |
| [Chiusura di F1 e F2 su QEMU: un'immagine i686 per T23, E500 e PC assemblati](2026-10-11-06-chiusura-f1-f2-su-qemu.md) | 11 ott | modifica (integrazione) e analisi |
| [Decisione: ripulitura automatica del bit dirty al mount, dopo la prima prova sul T23](2026-10-11-07-ripulitura-bit-dirty.md) | 11 ott | decisione (storage) e analisi hardware |
| [Prove su hardware: ThinkPad T23, sweep automatico e difetti reali corretti](2026-10-11-08-prove-hardware-t23-sweep.md) | 11 ott | analisi hardware e modifica |
| [Accelerazione: F3 e shell del desktop partono su QEMU in parallelo alla coda hardware di F0–F2](2026-10-11-09-accelerazione-f3-desktop.md) | 11 ott | decisione del proprietario e pianificazione |
| [Contratto F4 approvato: audio, media, volumi condivisi, ottico e NTFS in sola lettura](2026-10-11-10-contratto-f4-approvato.md) | 11 ott | decisione (contratto di progetto) |
| [Integrazione notturna: shell del desktop, nucleo VM DOS, audio AC'97, ATAPI/ISO 9660, NTFS, loader; qualifica QEMU e difetti corretti](2026-10-11-11-integrazione-notturna-qualifica-qemu.md) | 11 ott | modifica (integrazione) e analisi |
| [f3-03 completata dal lead: servizi di memoria XMS/EMS/UMB e primo avvio delle VM DOS su QEMU](2026-10-11-12-f3-03-memoria-vm-primo-avvio-dos-qemu.md) | 11 ott | modifica (implementazione e correzioni) e analisi di regressione |
| [Campagna QEMU dell'immagine integrata fcd96d6e; lavoro parallelo con subagent Claude verso la pre-alpha 0.9.0](2026-10-11-13-campagna-immagine-integrata-subagent.md) | 11 ott | modifica (correzioni) e analisi; decisione di metodo |
| [Lotto desktop fuso; campagne 9 e 10; fusione testuale di f3-05 e f3-04; agenti in parallelo verso la 0.9.0](2026-10-11-14-lotto-desktop-campagne-9-10-fusioni-f3.md) | 11 ott | modifica (fusioni e correzioni) e analisi di regressione; decisione di metodo |
| [Avvio normale bloccato da un livelock del lock VFS; fusioni notturne verso la 0.9.0; primi screenshot del desktop](2026-10-11-15-avvio-normale-livelock-fusioni-notturne.md) | 11 ott | modifica (correzione del kernel e fusioni) e analisi di regressione; decisioni |
| [Notte verso la 0.9.0: shell del desktop, icone Tango, avvio pulito, finestra DOS, DPMI, audio VM, DOOM; recupero degli accessori della 0.8](2026-10-12-01-notte-0-9-0-shell-icone-avvio-pulito-dos-giochi.md) | 12 ott | modifica (fusioni, correzioni) e analisi di regressione; decisioni |
| [Correzioni UI del proprietario, giochi DOS verdi, f3-07 e revisione del kernel](2026-10-12-02-correzioni-ui-giochi-f3-07.md) | 12 ott | modifica e verifica; decisioni f2-34, f3-08, giochi shareware |
| [Decisioni su identità e albero retrò; fusioni f2-34, f2-35, f3-08, sfondi Fill](2026-10-12-03-decisioni-identita-namespace-retro.md) | 12 ott | decisioni (design) e modifiche; qualifica QEMU in corso |
| [Integrazione completa 0.9.0: immagine 8435a16a e immagine T23 893a83e0](2026-10-12-04-integrazione-completa-immagine-t23.md) | 12 ott | fusioni finali, verifica host, consegna hardware; campagna QEMU rinviata |
| [Fusione di wt/integ2 in main: pre-alpha 0.9.0 integrata](2026-10-12-05-fusione-in-main.md) | 12 ott | fusione; decisione del proprietario (campagna interrotta) |
| [Scope: giochi Windows tramite API Win32 compatibile; immagine definitiva 36010476](2026-10-13-01-scope-win32-e-immagine-definitiva.md) | 13 ott | decisione (scope); fusioni finali e consegna hardware |
| [Consegne della notte dal secondo giro sul T23; immagine 8845b848](2026-10-13-02-consegne-notturne-immagine-8845b848.md) | 13 ott | modifiche (desktop, driver, DOS VM, audio); immagine per il T23 |
| [Contratto Win32 (F5) e decisioni; runner a due slot; diagnosi del blocco SuperSavage](2026-10-13-03-contratto-win32-decisioni.md) | 13 ott | decisioni (design); analisi hardware |
| [Terza prova sul T23 (immagine 3299a24f): diagnosi di accelerazione, disco, presentazione, DOOM e rete](2026-10-13-04-t23-immagine-3299a24f-diagnosi.md) | 13 ott | analisi hardware, decisioni (sette direttive Codex) |
| [Sette consegne Codex fuse; immagine 24c98799 per il T23](2026-10-13-05-sette-consegne-immagine-24c98799.md) | 13 ott | modifiche (kernel, desktop, test), immagine |
| [Tredici consegne fuse (prerequisiti F5 e parco driver); immagine 98c81726 per il T23](2026-10-13-06-tredici-consegne-immagine-98c81726.md) | 13 ott | modifiche (kernel, driver, desktop, SDK, test), immagine |
| [Quarta prova sul T23 (immagine e86305de): audio nativo attivo, UDMA, mouse USB; dieci direttive](2026-10-13-07-t23-immagine-e86305de-risultati.md) | 13 ott | analisi hardware, decisioni |
| [Immagine finale dell'ondata del 13 ottobre: 293c05af (ventitré consegne fuse)](2026-10-14-01-immagine-finale-293c05af.md) | 14 ott | modifiche (kernel, driver, desktop, SDK, test, runner), immagine |
| [Immagine 8c88b1ab: le nove consegne della mattina del 14 ottobre](2026-10-14-02-immagine-8c88b1ab.md) | 14 ott | modifiche (kernel, driver, desktop, runner, documentazione), immagine |
| [Immagine fb3147dc: giro di metà giornata del 14 ottobre](2026-10-14-03-immagine-fb3147dc.md) | 14 ott | modifiche (kernel, driver, VM DOS, runner), immagine |
| [Immagine 445e91c7: giro del pomeriggio del 14 ottobre (nove direttive)](2026-10-14-04-immagine-445e91c7.md) | 14 ott | modifiche (kernel, driver, VM DOS, desktop, giochi), immagine |
| [Immagine ce2e6ce9: giro della sera del 14 ottobre (sei direttive)](2026-10-14-05-immagine-ce2e6ce9.md) | 14 ott | modifiche (kernel, driver, desktop), immagine |
| [Immagine 66843edc: giro della notte del 14 ottobre (sei direttive)](2026-10-14-06-immagine-66843edc.md) | 14 ott | modifiche (kernel, driver, desktop, tema), immagine |
| [Revisione incrociata del contratto Win32 F5](2026-10-14-07-revisione-contratto-win32-f5.md) | 14 ott | analisi (fonti, contratti, dipendenze, verifiche host) |
| [Decisioni del proprietario della sera del 14 ottobre (DOS a schermo intero, Win16, MIDI, font, gate F5)](2026-10-14-08-decisioni-notte-f5.md) | 14 ott | decisioni |
| [Immagine 2e39fe38: giro della notte tra il 14 e il 15 ottobre (ricerche, latenza, video DOS, UI, avvio di F5)](2026-10-15-01-immagine-2e39fe38.md) | 15 ott | modifiche (kernel, driver, VM DOS, desktop, tema, font, F5), ricerche, immagine |
| [Correzioni della mattina del 15 ottobre: tasti del desktop, #XM assente su TCG, worker dello schermo intero; immagine a738204a](2026-10-15-02-correzioni-mattina-15-ottobre.md) | 15 ott | modifica (desktop, sonda Win32, display), analisi (deviazione QEMU), immagine |
| [Analisi del T23 sull'immagine a738204a: log dello schermo, DOOM, Quake, chiavetta, prestazioni](2026-10-15-03-analisi-t23-a738204a.md) | 15 ott | analisi (hardware reale), decisione (direttive) |
| [Immagine a4f67ac1: otto correzioni dall'analisi del T23 (log, Savage, chiavetta, DOOM, Quake, volume, desktop)](2026-10-15-04-immagine-a4f67ac1-otto-direttive.md) | 15 ott | modifica (kernel, driver, VM DOS, desktop), immagine |
| [Seconda analisi del T23 (immagine a4f67ac1) e immagine 5e018550: integrità del volume, Savage, chiavetta e rotellina, schermo intero, tema e sfondo, Paint, cursori, OpenGL software](2026-10-15-05-seconda-analisi-t23-e-immagine-5e018550.md) | 15 ott | analisi (hardware reale), modifica (kernel, driver, filesystem, desktop, SDK), immagine |
| [Terza prova del T23 (immagine 5e018550), cambio di metodo e immagine a395ffa8: volume che si ripara, scatola nera, pagina di autodiagnosi, mouse USB, seriale](2026-10-15-06-terza-prova-t23-metodo-e-immagine-a395ffa8.md) | 15 ott | analisi (hardware reale), decisione (metodo), modifica (filesystem, kernel, driver, loader), immagine |
| [Notte 15/16 ottobre: diagnosi completa dei log a395ffa8, cinque pacchetti notturni e immagine e8dfd0cd](2026-10-16-01-notte-diagnosi-cinque-pacchetti-immagine-e8dfd0cd.md) | 16 ott | analisi (hardware reale), decisione (metodo, driver), modifica (kernel, filesystem, driver, VM DOS, desktop, loader, runner), immagine |
| [Prova del T23 sull'immagine e8dfd0cd, cause misurate (PIC, chiavetta, Quake) e immagine a79f840c](2026-10-16-02-prova-t23-e8dfd0cd-cause-misurate-immagine-a79f840c.md) | 16 ott | analisi (hardware reale), decisione (architettura DPMI, registrazione), modifica (kernel, driver, filesystem, VM DOS, desktop, runner), immagine |
| [Prova del T23 sull'immagine a79f840c: ottica, chiavetta all'avvio, uscita dallo schermo intero, log perso, SMI; immagine d598bf4c](2026-10-17-01-prova-t23-a79f840c-ottica-chiavetta-schermo-intero-immagine-d598bf4c.md) | 17 ott | analisi (hardware reale), modifica (kernel, driver, filesystem, desktop, VM DOS, pagina di autodiagnosi), immagine |
| [Prova del T23 sull'immagine d598bf4c: blocco con la chiavetta, errore I/O a schermo intero, avvio Normale senza input; immagine finale 43d72a5c](2026-10-17-02-prova-t23-d598bf4c-chiavetta-schermo-intero-avvio-normale-immagine-43d72a5c.md) | 17 ott | analisi (hardware reale), modifica (kernel: filesystem, display, VM DOS, audio, avvio; desktop; runner), immagine |
| [Prova del T23 sull'immagine 43d72a5c: lag con la chiavetta, DOOM che spara, frame in finestra, seriale sparita; immagine 041b221f](2026-10-17-03-prova-t23-43d72a5c-chiavetta-doom-finestra-seriale-immagine-041b221f.md) | 17 ott | analisi (hardware reale), modifica (kernel: filesystem, VM DOS, audio, scheduler, loader, seriale; desktop e Files), immagine |
| [Prova del T23 sull'immagine 041b221f: linea IRQ 11 in quarantena, chiusura di DOOM a schermo intero, latenza ISR del guest, seriale disabilitata nel BIOS; immagine eca4d773](2026-10-18-01-prova-t23-041b221f-irq11-chiusura-doom-isr-seriale-immagine-eca4d773.md) | 18 ott | analisi (hardware reale), modifica (kernel: registro IRQ, rete, USB, VM DOS, display), immagine |
| [Prova del T23 sull'immagine eca4d773: schermo intero rifiutato dall'acceleratore, lettore MP3, velocità della chiavetta, nuova interfaccia; immagine 9af46936](2026-10-18-02-prova-t23-eca4d773-acceleratore-lettore-chiavetta-interfaccia-immagine-9af46936.md) | 18 ott | analisi (hardware reale), modifica (kernel: USB, display, acceleratore, registro; Music, libdeskui, desktop), decisione (interfaccia), immagine |
| [Prova del T23 sull'immagine 9af46936: DOOM a 17 Hz di IRQ0, trasporto audio fermo, nuova interfaccia; audit audio, affidabilità e barre; regressione audio VM trovata in QEMU e corretta (f4-32); immagine c2d2a237](2026-10-19-01-prova-t23-9af46936-doom-irq-trasporto-audio-f4-32-immagine-c2d2a237.md) | 19 ott | analisi (hardware reale e QEMU), modifica (kernel: audio, VM DOS, loader, USB, filesystem, display, registro, alimentazione; applicazioni: desktop, Music), decisione (audio, affidabilità, interfaccia), immagine |
| [Prova del T23 sull'immagine c2d2a237, prima cattura seriale: DOOM terminato allo stub IRQRET, stallo di 110 ms alla creazione delle VM, copia da chiavetta che satura il sistema; immagine 82fadce3](2026-10-19-02-prova-t23-c2d2a237-seriale-doom-irqret-stallo-vm-copia-immagine-82fadce3.md) | 19 ott | analisi (hardware reale), modifica (kernel: VM DOS, DPMI, creazione VM; applicazioni: Files), decisione (ritiro dei record ISR, punti di prelazione, trasferimenti su worker), immagine |
| [Decisioni del proprietario: gestione di disco, memoria, cache e tempo reale completa sul modello Windows 98/ME; gate QEMU sospesi, immagini direttamente sull'hardware](2026-10-19-03-decisioni-modello-win98-me-completo-gate-qemu-sospesi.md) | 19 ott | decisione (architettura, processo) |
| [Revisione incrociata del contratto "gestione delle risorse di classe Windows 98/ME" e sequenza delle direttive di implementazione](2026-10-19-04-revisione-contratto-gestione-risorse-win98-sequenza-direttive.md) | 19 ott | decisione (architettura), analisi |
| [Prova del T23 sull'immagine 82fadce3: copia che blocca il sistema, MP3 che balbetta, effetti di DOOM tagliati; writeback a budget, riserva del lettore, copia dal desktop su worker, frame SB in ritardo suonati; immagine 4bcd1d43](2026-10-19-05-prova-t23-82fadce3-copia-mp3-doom-sfx-writeback-riserva-immagine-4bcd1d43.md) | 19 ott | analisi (hardware reale), modifica (kernel: cache dei blocchi, sorgente audio VM; applicazioni: Music, desktop, Files, toolkit), immagine |
| [Decisione del proprietario: CiukiDOS resta il DOS di CiukiOS; le chiamate INT 21h su file passano al VMM (modello IFS Manager di Windows 9x); FreeDOS solo come riferimento di conformità e ospite di riserva](2026-10-19-06-decisione-ciukidos-resta-int21-file-nel-vmm-freedos-riferimento.md) | 19 ott | decisione (architettura DOS), analisi |
| [Mandato del proprietario: programma autonomo fino al completamento (contratto Windows 98/ME, API DOS nel VMM, F5 completo con installer e applicazioni Windows), nessuna immagine intermedia; media del Windows 98 Resource Kit forniti](2026-10-19-07-mandato-programma-autonomo-f5-completo-nessuna-immagine-intermedia.md) | 19 ott | decisione (processo, architettura, scopo di F5) |
| [Chiusura del tempo reale del contratto Windows 98/ME (R1, R2, R3): fusioni f0-44, f0-45 e f0-46 con l'emendamento sul recupero dell'orologio delle VM DOS](2026-10-19-08-chiusura-tempo-reale-r1-r2-r3-fusioni-f0-44-f0-45-f0-46.md) | 19 ottobre 2026 | integrazione |
| [C3 del contratto Windows 98/ME (I/O asincrono ed equità dei lettori) e API file DOS servita dal VMM: fusioni f1-83 e f3-49 con riconciliazione del VFS](2026-10-19-09-c3-io-asincrono-equita-lettori-api-file-dos-vmm-fusioni-f1-83-f3-49.md) | 19 ottobre 2026 | integrazione |
| [F5, nucleo della personalità Win32 integrato: loader e ntdll sul kernel, kernel32 parziale, servizio w32srv con registro, presenter e lease di presentazione, protocollo desktop v3 con i seam Win32](2026-10-19-10-f5-nucleo-personalita-win32-fusioni-f5-05b-f5-06-f5-07a-f5-08-f5-09.md) | 19 ottobre 2026 | integrazione |
| [Versione 0.9.2 pre-alpha: identità della versione aggiornata](2026-10-20-01-versione-0-9-2.md) | 20 ottobre 2026 | decisione |
| [Prima immagine integrata dopo la sospensione dei gate: regressioni di f0-44 trovate per bisezione e corrette, build canonico riparato, documentazione e catture della 0.9.2](2026-10-20-02-regressioni-f0-44-documentazione-e-catture-0-9-2.md) | 20 ottobre 2026 | integrazione |
| [Decisione del proprietario: i programmi Win16 con segmenti a 32 bit entrano nel perimetro di F5](2026-10-20-03-decisione-segmenti-32-bit-win16.md) | 20 ottobre 2026 | decisione |
| [Giochi DOS di nuovo avviabili dal menu: regressione di f3-51 trovata per bisezione e corretta](2026-10-20-04-regressione-giochi-dos-f3-51-corretta.md) | 20 ottobre 2026 | regressione |
| [Avvio rapido dei giochi DOS (f3-51c) e cursore del file di scambio (f0-49b)](2026-10-20-05-avvio-rapido-giochi-dos-cursore-fat.md) | 20 ottobre 2026 | modifiche |
| [Chiusura della sequenza Windows 98/ME (f3-46), MCI e AVI (f5-12B), ponte dei font (f5-10C2) e primo caso guest dei dialoghi Win32](2026-10-20-06-chiusura-sequenza-win98-mci-font-dialoghi-guest.md) | 20 ottobre 2026 | integrazione |
| [Prova T23 dell'immagine 3d18f2db (coda ATA bloccata a 512 byte), integrazione di f5-20D, f5-17B, f5-12B2, f5-15A, f5-07B, f5-07C e f5-22](2026-10-20-07-prova-t23-coda-ata-integrazione-f5-07-f5-22.md) | 20 ottobre 2026 | analisi hardware e integrazione |
| [Decisioni del proprietario verso la 1.0 Beta: rete in parallelo, QEMU per rete e 3D, OpenGL, Win16 dopo la beta](2026-10-20-08-decisioni-verso-1-0-beta.md) | 20 ottobre 2026 | decisione |
| [Analisi delle due prove T23 fallite: finestra DOS che affama la VM, ridisegno continuo di Files, contesa su C:](2026-10-20-09-analisi-prove-t23-cause-lag-e-giochi-dos.md) | 20 ottobre 2026 | analisi hardware |
| [Integrazione del 21 ottobre: correzioni del T23, osservatori guest, file di scambio, pin del toolchain; prima immagine completa dopo le revisioni incrociate](2026-10-21-01-integrazione-correzioni-t23-osservatori-guest-prima-immagine-completa.md) | 21 ottobre 2026 | integrazione |
| [Prova del T23 sull'immagine b51f034f: prenotazioni del volume di 50–1500 ms, refill audio in coda, VM in finestra accoppiata al desktop; direttiva f1-87](2026-10-21-02-prova-t23-b51f034f-prenotazioni-volume-refill-audio-vm-finestra.md) | 21 ottobre 2026 | analisi (hardware reale), decisione (direttiva) |
| [Decisione del proprietario: unificare CiukiDOS e FreeDOS in un unico DOS definitivo e completo; direttiva f3-54](2026-10-21-03-decisione-unificazione-ciukidos-freedos.md) | 21 ottobre 2026 | decisione (architettura DOS) |
