# Stream B Smoke Tests (Documentali) - 2026-05-01

## Scope
Questi smoke test coprono funzionalita interattive della shell Stage1 introdotte in Stream B:
- P5 parser argomenti con virgolette
- P6 editor riga avanzato
- P7 command history circolare
- P8 tab completion baseline

I test automatici QEMU eseguiti in pipeline validano boot/runtime, ma non coprono in modo diretto l'input interattivo tasto-per-tasto.

## Ambiente
- Profilo floppy: `./scripts/qemu_run_floppy.sh`
- Profilo full: `./scripts/qemu_run_full.sh`

## P6 - Editor riga avanzato
1. Digitare `abc`, premere `Left` due volte, digitare `X`.
- Atteso: riga diventa `aXbc` senza corruzione caratteri.
2. Premere `Home`, digitare `Z`.
- Atteso: riga diventa `ZaXbc`.
3. Premere `End`, poi `Backspace`.
- Atteso: elimina ultimo carattere (`ZaXb`).
4. Portare cursore in mezzo con `Left`, premere `Delete`.
- Atteso: elimina il carattere sotto cursore e riallinea la coda.

## P7 - History comandi (N >= 4)
1. Eseguire almeno 5 comandi semplici, ad esempio:
- `ver`
- `pwd`
- `dir`
- `help`
- `ticks`
2. Premere `Up` ripetutamente.
- Atteso: richiamo in ordine inverso degli ultimi comandi (buffer circolare, almeno 4 voci).
3. Su un comando richiamato, modificare testo (es. aggiungere carattere) prima di invio.
- Atteso: editing consentito senza blocchi.
4. Premere `Down` fino a uscire dalla history.
- Atteso: ritorno alla riga di editing corrente salvata.

## P5 - Quote parsing
1. Eseguire: `md "A B"`
- Atteso parser: token singolo `A B` passato al comando `md`.
2. Eseguire: `cd "A B"`
- Atteso parser: token singolo `A B` passato al comando `cd`.
3. Eseguire: `copy "SRC FILE" "DST FILE"`
- Atteso parser: due token distinti, ciascuno con spazi interni.
4. Eseguire: `run "MY APP"`
- Atteso parser: argomento unico, con tentativo `.COM/.EXE` se privo estensione.

Nota: il supporto parser con virgolette e indipendente dal successo filesystem/lookup del path.

## P8 - Tab completion baseline
1. Digitare prefisso built-in univoco (es. `pw`) e premere `Tab`.
- Atteso: completamento a `pwd`.
2. Digitare prefisso ambiguo (es. `r`) e premere `Tab`.
- Atteso: input invariato, nessuna corruzione.
3. Digitare prefisso file eseguibile (es. `COMD`) e premere `Tab`.
- Atteso: completamento verso nome `.COM/.EXE` univoco trovato via findfirst/findnext.

## Risultato atteso globale
- Nessuna corruzione della riga durante navigazione/edit.
- Nessun crash shell con Up/Down/Home/End/Delete/Tab.
- Parsing quoted stabile su tutti i comandi target.
- In caso di ambiguita completion, input lasciato intatto.
