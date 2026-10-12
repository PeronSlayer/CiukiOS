# Decisioni del proprietario su identità e albero dei file; fusioni del pomeriggio (f2-34, f2-35, f3-08, sfondi Fill)

- **Data:** 2026-10-12
- **Tipo:** decisioni (design) e modifiche; verifica QEMU
- **Versione/commit:** `wt/integ2` da `5f8a09e2` a `120b4d89`
- **Stato:** fusioni fatte; immagine in costruzione e qualifica QEMU in corso

## Contesto

Dopo gli screenshot dell'immagine `2fdd515a…` il proprietario ha dato
quattro indicazioni: togliere la tagline dalla barra in alto e sostituirla
in About; mettere i suoi sfondi della vecchia baseline con `Ciuk2`
predefinito; un solo set di icone fisso e coerente, con etichette leggibili
sullo sfondo; e, sull'albero dei file visto in Files (`bin`, `dev`, `home`,
`mnt`, `system`, `tmp`), la domanda su chi l'avesse deciso. L'albero era
nel contratto F2 (`docs/design/posix-subset.md`, 10 ottobre, scritto da
Codex e approvato dal lead come conseguenza del sottoinsieme POSIX deciso il
9 ottobre); il proprietario non era stato interpellato sulla parte visibile.

## Decisioni

1. **Tagline**: `Classic computing, one woof at a time.` sostituisce
   `A modern Retro OS` in About, nelle finestre About delle app, nel README,
   nelle note del marchio e in `AGENTS.md` (`f1cedd38`). La barra in alto
   non mostra più la tagline (`5ca58877`).
2. **Sfondi**: le tre scene adattive `Ciuk1/2/3` (1448×1086, da
   `misc/ciukios_bg/adaptive`, hash in `generation.json`) entrano
   nell'immagine come `assets/wallpapers/ciuki` → `/system/wallpapers`;
   `Ciuk2` è il predefinito (`26503023`). Il ritaglio 1:1 centrato tagliava
   la scritta e la coda del cane: aggiunta la modalità `fill` (scala
   uniforme a coprire l'area fra le barre, ritaglio centrato, stesso
   ricampionamento di stretch) come modalità predefinita; Control Panel con
   cinque posizioni (`27bda483`, fusione `cfaa7949`). La selezione di un
   file non forza più Tile. Euristica `normal-boot-desktop-3`: l'area di
   lavoro deve solo essere priva dei colori della console (misurato 0,6 %
   sul desktop predefinito, limite 2 %).
3. **Albero dei file in stile retrò** (proposta del lead accettata dal
   proprietario): la vista POSIX resta per i programmi, ma `/bin` diventa
   `/Programs` e `/home` diventa `/Documents`; `/system` e la cartella dei
   giochi sono creati con i nomi lunghi `System` e `Games` (percorsi
   case-insensitive e case-preserving, contratto VFS); Files parte da
   "Computer" con le unità `C:`, `D:`… (`/mnt/<lettera>`), mostra i
   percorsi come `C:\Programs`, nasconde `dev`, `mnt` e `tmp`. Direttive
   `f2-35-retro-namespace.md` e `f2-36-files-computer-view.md`;
   emendamento del contratto in `posix-subset.md`.
4. **Codex in parallelo**: su richiesta del proprietario, quattro sessioni
   Codex contemporanee (`gpt-6.1-sol`, xhigh per le implementazioni, high
   per le risoluzioni di conflitti), a priorità bassa (`nice -n 19`) per
   non disturbare le suite QEMU.

## Cosa cambia (fusioni in `wt/integ2`)

- `a841cd88` **f2-34**: ridimensionamento delle finestre con superfici di
  sostituzione allocate dal client (nessuna modifica al kernel o all'ABI
  delle syscall; protocollo desktop v2.1: CREATE con limiti e passi,
  CONFIGURE con seriale, DAMAGE con la superficie sostitutiva), bordi e
  angolo, zoom all'area di lavoro, relayout del toolkit nelle app.
- `2da0ff95` **f3-08**: correzioni dagli otto rilievi della revisione del
  kernel, ciascuno riprodotto con un test host prima della correzione:
  trap dell'IF virtuale DPMI legato a DR6.BS con armamento monouso e
  isolamento dei figli; firma PUSHF/POP decodificata in avanti da un
  confine noto; preflight della raffica VGA prima di ogni effetto (un REP
  abbandonato non ripete un elemento già eseguito) e forme LOCK rifiutate;
  `ciuki_power` ripete un sync fallito e non agisce mai su errore, `_S5`
  letto con PkgLength e validazione dello scope radice, SCI_EN richiesto in
  PM1a/PM1b prima delle scritture PM con ripiego sul riavvio; coda audio
  che si ferma solo su silenzio provato. Documento
  `docs/validation/2026-10-12-f3-08-host.md`.
- `120b4d89` **f2-35**: rinomina `/bin` → `/Programs`, `/home` →
  `/Documents` in kernel (avvio del supervisore, sonde), SDK, app,
  costruttore d'immagine, runner (pietre miliari dell'avvio normale), suite,
  test e documenti; 86 file.
- `8b434e11` runner: la scadenza di 10 s della risposta QMP solleva
  `TimeoutError` e il caso è classificato come timeout (regressione del
  test `test_timeout_identifies_pending_cut_and_arm`, fase qmp).
- `f2-36` (vista Computer di Files) consegnata da Codex; fusione con nove
  conflitti in corso di risoluzione.

## Verifiche

Immagine `2fdd515a…` (`5f8a09e2`, con f3-07): `zz-games` 3 PASS (DOOM,
DOOM con audio, Wolf3D con la finestra lineare), `f0-smoke` 4 PASS,
`desktop-shell-qemu-t23` PASS; `files-qemu-t23` FAIL con `keys=0` (108
eventi consegnati, 39 movimenti e 16 pulsanti contati, nessun tasto): lo
stesso sintomo di `desktop-shell-qemu-e500`; causa in analisi sul replay
host (worktree `wt/shell-keys2`). Il messaggio del runner "invalid desktop
interaction fields" per questo caso è fuorviante (l'errore di contratto
viene riavvolto come errore di campo).

Test host: 53 test del desktop verdi con Fill; `host_kernel_tests.sh`
verde sull'albero f3-08 fuso con f3-07; 75 test desktop verdi dopo la
risoluzione dei conflitti di f2-34.

## Aperti

Pacchetto Wolfenstein 3D shareware: `VENDOR.DOC` [3][C] richiede il permesso
scritto di Apogee per l'inclusione in un pacchetto software; decisione del
proprietario prima di qualsiasi pubblicazione.
