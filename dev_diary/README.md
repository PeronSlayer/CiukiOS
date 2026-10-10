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
