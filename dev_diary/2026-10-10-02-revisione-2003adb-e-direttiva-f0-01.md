# Revisione del commit 2003adb e direttiva f0-01

- **Data:** 2026-10-10
- **Tipo:** analisi e modifica
- **Versione/commit:** `866e5f1`
- **Stato:** correzione dello script completata; le altre correzioni in
  corso (direttiva f0-01)

## Contesto

Il commit `2003adb` (chiave `safe=1`, selettore `core`, script
`write_physical.sh`) era stato integrato il 9 ottobre senza revisione
incrociata. La revisione è stata fatta il 10 ottobre da Codex
(`gpt-6.1-sol`, effort `high`, sola lettura) rispetto a `f0-acceptance.md` e
`boot-memory.md`.

## Esito della revisione

Nove punti, in ordine di gravità:

1. `write_physical.sh` accettava come conferma qualsiasi sottostringa del
   percorso by-id: una sola lettera bastava.
2. `core` e `all` ignorano l'esito di ogni sonda e continuano dopo un
   fallimento; il contratto richiede di fermarsi e registrare le sonde
   successive come non eseguite.
3. La cronologia dello schermo (512 righe) non garantisce di conservare
   tutta l'evidenza di `core` nel caso peggiore (128 voci E820 stampate due
   volte); il log del run manuale (~482 righe) ci sta.
4. Il runner costruisce la richiesta safe-mode per il profilo E500
   nell'ordine sbagliato (`safe=1 platform=e500`).
5. Nel loader un modo video configurato ha priorità sulla modalità
   provvisoria, al contrario del modello di riferimento; l'immagine corrente
   non configura alcun modo.
6. Il parser del kernel accetta nomi malformati (NUL incorporati) e non
   verifica la coerenza tra suffissi e flag di avvio; il loader attuale li
   rifiuta.
7. I predicati del timer in `f0-core.json` passano anche senza progresso del
   timer.
8. Il runner non rifiuta più `loader_options` nei casi delle suite.
9. Il menu del loader non mostra a schermo i caratteri digitati del
   selettore (solo su UART).

Nessun errore di segmento SS/DS trovato nel loader; ordine, duplicati e
limite di 64 byte del parser del loader sono corretti.

## Cosa è stato fatto

- Punto 1 corretto da Claude (correzione urgente e verificabile subito): il
  dispositivo deve essere un disco intero, il suo numero di serie letto da
  `lsblk` deve far parte del nome stabile e la conferma digitata deve
  coincidere esattamente con il numero di serie. Verificato sul disco di
  prova.
- Punti 2–9 affidati a Codex (`gpt-6.1-sol`, effort `high`) con la direttiva
  `docs/directives/f0-01-review-fixes.md`, nel worktree `wt/f0-review-fixes`.
  Accettazione: test host, build del kernel con audit FPU, suite `f0-smoke`,
  `f0-core` (con il caso safe-mode) e `f0-panic` su QEMU, poi nuova immagine
  per l'hardware.

## Decisioni

- L'immagine `ciukios-2003adb0.img` resta utilizzabile per una prima prova a
  schermo; la qualificazione T4 richiede l'immagine con le correzioni.
- Nessun commit su `main` senza revisione incrociata, salvo correzioni
  urgenti verificabili subito, che vanno registrate nel diario.
