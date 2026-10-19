# Mandato del proprietario: programma autonomo fino al completamento (contratto Windows 98/ME, API DOS nel VMM, F5 completo con installer e applicazioni Windows), nessuna immagine intermedia; media del Windows 98 Resource Kit forniti

- **Data:** 2026-10-19
- **Tipo:** decisione (processo, architettura, scopo di F5)
- **Versione/commit:** 0.9.0 pre-alpha / main `a12b4380`
- **Stato:** in vigore

## Contesto

Alle 14:40, con cinque sessioni Codex in corso (f0-44, f0-46, f4-36, f3-47, f3-48) e l'immagine `4bcd1d43` in prova, il proprietario ha disposto di procedere con tutte le implementazioni in autonomia, usando Codex e più agenti, senza altre immagini intermedie, fino al completamento: la sequenza del contratto "gestione delle risorse di classe Windows 98/ME" (voce 2026-10-19-04), l'API file DOS nel VMM (voce 2026-10-19-06) e il passo F5 completo, inteso come apertura degli eseguibili Windows sia come installer sia come applicazioni vere, non solo giochi. Per chiudere le sette domande di ricerca dell'audit f0-42 ha fornito i media originali del Windows 98 Resource Kit in `third_party/mswin98rskt/` (CD del Resource Kit: libro completo `HELP/RK98BOOK.CHM`, strumenti e documentazione; 169 MiB, materiale Microsoft non ridistribuibile, escluso dal repository tramite `.gitignore`).

## Decisioni

1. Il lead conduce il programma in autonomia: direttive, revisioni, fusioni, suite host e build canoniche senza chiedere conferme intermedie; riferisce al proprietario per eccezioni, disaccordi fra agenti e al completamento di ogni milestone. Nessuna immagine viene prodotta per la prova sull'hardware finché il proprietario non la richiede o il programma non è completo; ogni fusione resta comunque accompagnata da suite host verdi e da una build canonica dell'albero fuso.
2. Scopo di F5 ampliato: oltre ai giochi Windows 1996–2004 del contratto `docs/design/win32-personality-contract.md`, gli installer originali (InstallShield, Inno Setup, NSIS, setup Microsoft) e le applicazioni Windows dell'epoca devono aprirsi e funzionare; la sequenza f5-04 → f5-18 del contratto viene eseguita per intero, con f5-14B/C (riuso dei DLL superiori di Wine 10.0, già prescelto in f5-14A) e f5-15 (flussi degli installer e integrazione con la shell) ampliati alle applicazioni.
3. Le sette domande storiche aperte (voce 2026-10-19-04) si chiudono con i media forniti: direttiva f0-42b, che legge il libro del Resource Kit estratto in `build/rskt/` e la documentazione del CD, registra edizione, capitolo e fatto per ciascuna, e aggiorna i numeri del contratto che ne dipendono.
4. Parallelismo: più sessioni Codex contemporanee su worktree con file disgiunti; la memoria del posto di lavoro (15 GiB) limita a circa sei sessioni; il lead serializza le fusioni e rilancia la direttiva successiva alla consegna di ciascuna.

## Riferimenti

- Voci 2026-10-19-03, -04, -05, -06; `docs/design/win98-class-resource-management.md`; `docs/design/win32-personality-contract.md` (direttive f5-02…f5-18 e tabella delle dipendenze); `docs/directives/README.md`.
