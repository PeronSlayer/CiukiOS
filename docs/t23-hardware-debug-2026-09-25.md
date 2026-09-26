# T23: blocco dopo la splash — 25 settembre 2026

## Stato fisico confermato

L'ISO masterizzata `7f249a1e4d2cfd207e7b50a62cbf708355955f939bd79f694018c506717f80f0`
non è qualificata sul T23. Il proprietario riferisce:

- Live normale: beep, cursore fermo dopo la splash, poi caratteri corrotti.
- Safe graphics: inizialmente sembrava fermo nel caricamento, ma **alla fine
  raggiunge il desktop**. Non registrare il primo resoconto come blocco certo
  del caricatore.
- F4 da Safe apre la console e accetta `sound test`; dopo Invio si blocca,
  senza che sia stato osservato `[SOUND] Testing PCM playback...`.
- Alternate disk access: cursore dopo la splash e, dopo un'attesa, testo
  corrotto. Cambia il trasferimento del disco RAM, ma esegue ancora l'audio.
- Non è disponibile un risultato fisico attendibile di COMDEMO in questa
  sessione Safe. Non dedurre dal prompt che EXEC funzioni sul T23.

La mancata osservazione del primo messaggio richiede di controllare anche
EXEC, ridimensionamento memoria e output, prima di attribuire il blocco
all'AC97. Il vecchio BOOTSND non conteneva un fallback a beep: quei suoni non
dimostrano che il player sia terminato. La causa fisica resta aperta.

## Correzioni dimostrate e isolamento

1. BOOTSND abilita ora la sola decodifica PCI I/O prima di leggere lo stato
   occupato dell'uscita; il bus mastering viene abilitato successivamente.
   Il comando PCI originale è ripristinato anche sulle uscite anticipate.
2. Le letture e scritture AC97 attendono CAS con limite finito e propagano
   l'errore. ICH3 descrive il protocollo nel
   [datasheet Intel, sezione 5.18.1.23](https://www.intel.com/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf).
3. `SOUND TEST` non scrive più automaticamente nell'Embedded Controller.
   L'accesso grezzo esistente rimane una diagnostica esplicita `/E`; `/Q`
   lo esclude. Il
   [driver Linux ThinkPad](https://github.com/torvalds/linux/blob/v6.12/drivers/platform/x86/thinkpad_acpi.c)
   usa i servizi EC, il cui
   [driver](https://github.com/torvalds/linux/blob/v6.12/drivers/acpi/ec.c)
   serializza le transazioni e gestisce il global lock quando richiesto.
   CiukiOS non implementa questa gestione condivisa del firmware.
4. `SOUND /D` mostra le fasi su BIOS video e seriale, partendo **prima della
   prima chiamata DOS del player**. Non dipende da AH=09h per il primo marker;
   conserva registri e FLAGS, limita l'attesa seriale e non stampa durante DMA.
5. GRUB aggiunge, in fondo al menu, `Live CD (no startup sound)`: stessa
   grafica normale, nessuna esecuzione automatica di BOOTSND. Il flag `MUTE`
   viene consumato nel disco RAM; gli indici precedenti e recovery=5 restano.
6. AUTO senza EDID preferito valido è limitato a 800×600. EDID valido e
   selezione esplicita delle risoluzioni superiori continuano a funzionare.
   Il limite del controller VBE non identifica il limite del pannello LCD.
7. Stage1 conserva DI nel fallback EDD→CHS: AH=08h può restituire ES:DI e
   prima distruggeva l'indice della FAT. Dimensione 1542→1544 byte; patch
   della lettera disco di Setup sempre all'offset 23. È un difetto distinto
   dal blocco dopo la splash.

Nessuno di questi difetti viene presentato come causa fisica già accertata.
Non sono stati introdotti workaround S3 o modifiche speculative al kernel.

## Prove riproducibili

Le prove CPU sono esecuzioni di istruzioni reali con risposte periferiche
modellate, non test su schede fisiche:

- `scripts/test_ac97_init_cpu.py`: 18 casi, inclusi I/O disabilitato, codec
  occupato, errori PCI/allocazione, diagnostica e EC. Il vecchio player falliva
  i controlli negativi di decodifica I/O e scritture codec accavallate.
- `scripts/test_stage1_chs_fat.py`: 64 casi; il controllo negativo ricrea
  esattamente il vecchio Stage1 masterizzato e legge cluster 0 invece di 3.
- `scripts/test_vbe_auto_bounds.py`: 11 casi EDID/profili; altri 7 gruppi di
  regressione VBE passano.
- Audit del kernel masterizzato: 20 casi caricano integralmente COMDEMO,
  SOUND, BOOTSND, AUXSTACK e SHELL dai settori reali, anche con fallback CHS
  e registri BIOS alterati. Non copre ricerca del percorso, allocatore e
  ingresso nel figlio sul T23.
- Il test di stack BIOS video al primo avvio passa con pressione sintetica
  di 1024 byte; il controllo negativo da 2048 rileva la corruzione. Non è una
  misura dello stack del BIOS T23 e non giustifica un aumento alla cieca.

Artefatti dei controlli iniziali: `build/full/t23-audio-audit-2026-09-25/`,
`build/full/t23-first-boot-2026-09-25/`, `build/full/t23-lfb-review-2026-09-25/`.

## Candidato e prova fisica successiva

Candidato isolato: `build/full/t23-followup-2026-09-25/`.
ISO: `cd/CiukiOS_full_cd_0-7-1.iso`, 149397504 byte,
SHA256 `90dae91c1ada563bf64d94ec0dc1541860915c60060e213c62f48299b75c2c77`.
`source-freeze.json` e `packaged-binaries.json` collegano sorgenti, oggetti e
file effettivamente estratti dalla FAT. La vecchia ISO masterizzata e i file
CD canonici precedenti sono conservati. Su richiesta del proprietario, questo
candidato è stato masterizzato sul CD-RW in `/dev/sr0` il 25 settembre 2026:
operazione completata alle 19:54:24 UTC, 149397504 byte scritti, uscita 0,
espulsione richiesta e nessuna rilettura completa. Il dispositivo ha usato
circa 10× nonostante la richiesta di 4×. Log e orari: `burn-cdrw.json` nella
directory del candidato.

Il successivo riscontro fisico del proprietario su questo CD è:

- `Live CD (no startup sound)` raggiunge un desktop utilizzabile con mouse
  e tastiera.
- `comdemo` provoca un rapido cambio schermo e ritorna alla console. Non è
  confermata la lettura del messaggio del programma: questo resoconto non
  dimostra né un EXEC fallito né una corretta esecuzione completa.
- `sound /d` non mostra un messaggio osservato e poi riavvia il T23.

Anche questo candidato **non è qualificato sull'hardware**. Un messaggio
non osservato potrebbe essere stato cancellato dal cambio modalità video;
non basta per localizzare il guasto prima dell'ingresso nel player.
Il proprietario ha quindi richiesto una revisione delle fondamenta senza
ulteriori prove in emulazione. Le modifiche successive e i loro limiti sono
nel [rapporto della revisione](t23-foundation-audit-2026-09-25.md).

I risultati QEMU del candidato sono nei report per sessione sotto la stessa
directory. Il limite della sessione VS Code rimane 8.000.000.000 byte.

Risultati in emulazione precedenti alla revisione successiva, sull'ISO
`90dae91c…`, senza sostituire file nel guest,
con Pentium III e 128 MiB:

| Verifica | Esito | Evidenza nel candidato |
|---|---|---|
| Live, Setup, DOS, Safe, nuovo avvio senza suono | 5 sessioni passate; Setup 800×600; nuovo avvio silenzioso | `cd-sessions/result.json` |
| EDID assente | Primo desktop 800×600, poi VGASETUP confermato a 2560×1440 | `missing-edid/result.json` |
| EDID valido 1024×768 e 2560×1440 | Risoluzione rispettata; COMDEMO e ritorno al desktop | `edid-{1024,2560}/result.json` |
| AC97 Live | PCM al boot RMS 3573,78; SOUND /D RMS 3540,79; COMDEMO e ritorno | `cd-ac97-diagnostic/results.json` |
| AC97 Safe | Boot silenzioso; SOUND /D RMS 3573,78; COMDEMO e ritorno | stesso report, caso `safe` |

Le fasi diagnostiche sono state catturate visivamente durante l'esecuzione,
oltre ai marker seriali. Tutte le VM di verifica sono chiuse e l'hash ISO
è invariato. Questi risultati non certificano il firmware o l'audio del T23.

Per i download e la compilazione dei driver: [ricerca T23](t23-drivers-2026-09-25.md).
