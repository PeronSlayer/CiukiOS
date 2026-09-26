# CiukiOS UI — debug e redesign del 25 settembre 2026

**Stato: build di sviluppo, lavoro non completo.** Il requisito di fluidità a
2560×1440 non è ancora raggiunto. Il cambio fra immagini complete è verificato
in QEMU con doppia pagina video, ma massimizzare/ripristinare a questa risoluzione
richiede ancora circa 2,4 secondi sulla macchina di prova. La compatibilità
effettiva con ATI Radeon 9200 e le prestazioni di un Pentium III fisico restano
da misurare. Il profilo CPU emulato non riproduce la velocità di un Pentium III.

## Cambiamenti implementati

- Desktop e pannelli platino, titoli indaco con righe orizzontali, fondo verde
  petrolio, icone a pixel e font proporzionale bitmap a dimensione nativa.
  Testi in inglese e tagline `A modern Retro OS`; la fotografia di Ciuki
  all'avvio non è stata sostituita. Anche la cornice del Setup usa il tema.
- Focus tastiera conservato separatamente per ogni finestra. Prima della
  correzione, Run → About → Alt+Tab → Invio poteva chiudere Run invece di
  eseguirne il comando. La ricerca del focus considera anche la finestra
  proprietaria; Close nel titolo e Cancel nel pannello hanno ID distinti.
- La selezione ridisegna anche l'elemento precedentemente selezionato; un click
  non lascia due evidenziazioni. Un pulsante premuto e rilasciato su un controllo
  di un'altra finestra non attiva più l'azione sbagliata.
- Pulsanti con stato premuto, breve accento mobile del titolo su quattro tick
  BIOS, nessuna attesa attiva per l'animazione. Power, errore e menu temporaneo
  non espongono più Minimize senza un modo per ripristinarli.
- Aree da ridisegnare limitate alle finestre interessate e taskbar aggiornata
  separatamente. La preferenza audio viene letta all'ingresso nella UI, invece
  di aprire un file ad ogni frame.
- Buffer di composizione da 60 KiB, impacchettato sulla larghezza dell'area
  modificata; controlli dei limiti, pitch, banchi e pixel a cavallo dei 64 KiB.
  Copie sequenziali senza confrontare ogni dword con la VRAM.
- Presentazione opzionale su due pagine VBE: una pagina è preparata prima del
  cambio visibile; il cursore resta associato alla pagina corretta. Memoria,
  numero di pagine, scanline e set/get display-start sono verificati. Errori
  richiedono una nuova sessione video con il percorso opzionale disabilitato.
- Modi nativi 1280×1024, 1600×1200, 1920×1080, 2048×1152 e 2560×1440 vengono
  cercati nella lista effettiva del BIOS; nessun ID di modo inventato. Il nuovo
  comando `VGASETUP DESKTOP <larghezza>` offre anteprima, annullamento e timeout
  di 12 secondi, preservando il driver Windows 3.1. I vecchi comandi condivisi
  `VGASETUP SET` conservano il loro significato.
- Annullare il Setup avviato da GRUB o entrare direttamente in DOS non impone
  più 640×480 al successivo `EXIT`. La modalità di emergenza rimane una scelta
  esplicita separata. Il difetto è stato riprodotto prima della correzione.

Kernel, COMMAND.COM, launcher Windows, motore Doom e BOOTSND nella nuova
immagine QEMU sono byte-identici a quelli della precedente immagine stabile;
il confronto è in `development-core-diff.json`. La build ricompila inoltre i
componenti esterni previsti dal processo esistente: non si assume identità di
ogni file non elencato in quel confronto.

## Prove ed evidenze

Tutte le evidenze di questa attività sono in
`build/full/ui-redesign-2026-09-25/`. I test lavorano su copie delle immagini.

| Prova | Risultato osservato |
| --- | --- |
| Renderer assemblato eseguito in Unicorn | 14 gruppi passati: cinque profondità, clipping, pitch, bordi dei banchi, guardie, profili, pagine e recupero dagli errori. Non è una prova su GPU fisica. |
| Tre regressioni di selezione/focus/rilascio | Riprodotte nella candidate-2, corrette e rieseguite con successo nella candidate-3. Immagini e fallimenti conservati. |
| Finestre a 800×600 e 1024×768, 128 MiB | Sovrapposizione, testo Run conservato, trascinamento, resize, minimizzazione/ripristino, cursore, Caps Lock, COMDEMO e ritorno DOS/UI verificati. |
| Cambio pagina candidate-5 a 2560×1440×32, VGA 32 MiB, RAM 128 MiB | 55 frame durante massimizzazione e 56 durante ripristino: ogni frame corrisponde all'immagine vecchia o nuova completa; zero frame misti e zero danni residui fuori cursore/orologio. |
| Latenza 2K nella stessa prova | 2,300 s / 2,357 s: requisito di fluidità **non soddisfatto**. |
| Riposo a 1024×768 | Quattro catture senza modifiche fuori orologio; zero marker PAINT in 2,06 s; circa 0,02 s CPU host QEMU. Non è un benchmark Pentium III fisico. |
| Modi video | 1280 annullato; 1920 annullato dal vero timeout di 12,03 s; 2560 confermato e applicato a DOS/UI; SYSTEM.INI di Windows identico. |
| VRAM insufficiente | Richiesta 2560 rifiutata con VGA 2 MiB; preferenze e Windows preservati; COMDEMO ancora eseguibile. |
| Windows nella build precedente agli ultimi due fix UI, SB16, 128 MiB | Due sessioni, applicazioni effettive, Calculator, ridimensionamento/ridisegno, WAV/MIDI e uscita superati. Zero pixel residui nella verifica di ridisegno. Artefatto `before-final-development.img`. |
| ISO finale, solo CD, 128 MiB | Desktop raggiunto in 13,973 s, COMDEMO in DOS e ritorno UI. Nessun HDD collegato. |
| Finestre candidate-5, 800×600 e 2560×1440, 128 MiB | Suite completa passata, comprese navigazione Tab corretta, minimizzazione, bordi del cursore, DOS, Caps Lock, COM e ritorno. Il superamento funzionale non certifica la fluidità. |
| Build finale: Wolf3D → Doom Vanille → Windows → DOS, SB16, 128 MiB | Partite, movimento, uscita, COM tra i giochi, avvio Windows e ritorno nella stessa VM. PCM effettivo nei giochi (RMS 1359,5 e 1539,0) e suono di avvio presente. |
| ISO finale: Live, Setup → Esc → EXIT, DOS → EXIT | Tre percorsi passati a 128 MiB con desktop completo e stabile a 1280×800; Setup realmente a 800×600. La correzione del flag SAFE è verificata. |
| ISO finale: SAFE | Mantiene 640×480; il controllo visivo fallisce perché il primo ridisegno planare è ancora parziale dopo 1 s e termina nella cattura successiva. Fallimento conservato, non dichiarato passato. |
| Doom classico, candidate-5, SB16, 128 MiB | Menu, partita, movimento, uscita e COM funzionano. Il mouse al ritorno UI non risponde entro 300 ms: lo stesso pacchetto appare dopo circa 777 ms, senza reinvio. Funzionalità osservata, requisito di risposta fallito. |

I dettagli del collaudo finestre, inclusi i tentativi del test che hanno letto
uno stato ancora in elaborazione e le successive correzioni del test, sono in
[ui-validation-2026-09-25.md](ui-validation-2026-09-25.md). Un marker PAINT
attesta la fine del lavoro; l'assenza di immagini miste è controllata sui
pixel effettivi. Nessun timeout di fluidità viene convertito in un successo
solo perché l'immagine finale arriva corretta.

Altri rapporti: [rendering](ui-rendering-2026-09-25.md),
[design e revisione delle schermate](ui-design-2026-09-25.md),
[ritardo del mouse dopo Doom](ui-doom-pointer-diagnostic-2026-09-25.md).
Il file `qualification.json` nella cartella delle evidenze riepiloga gli esiti,
le identità delle immagini e le verifiche ancora mancanti. La prima prova Doom
con il vecchio riconoscimento dei colori è conservata: lo stesso PNG contiene
il cursore corretto col nuovo tema (`pointer-palette-diagnosis.json`). Dopo aver
corretto quella sola assunzione del test, la nuova prova ha rivelato il ritardo
reale sopra descritto; non è stata registrata come successo completo.

## Artefatti e riproduzione

- `development.img`: immagine QEMU compilata dai sorgenti correnti.
- `development.iso`: ISO di sviluppo con live e Setup; SHA-256
  `fa70d79d97a9fa2df9862b72d525e51eccc54bc22fcbd24d3be776c09a1db383`.
- `development-cd-source.img`: sorgente disco del CD; SHA-256
  `1398b400d56f9ccfb12b7198bd3a89f2f0634c10e528bad6f4fdf8ad0f532c0e`.
- `development-artifacts.json`: identità degli artefatti; i risultati dei test
  registrano separatamente i binari realmente caricati.

La shell finale ha SHA-256
`82933833f649d7941a6cfc54da220d8d6232d4fb7e74fef30797e22530ee1acb`.
La candidate-5, usata per il collaudo finestre, precede solo la rimozione del
flag SAFE dal ramo DOS/annullamento Setup: SHA-256 shell
`4449cd0384077f5e81251dde24d87a79cbc0f0124419a3fcb1d6a067c6313d19`.
Gli artefatti precedenti sono conservati con prefisso `before-final-`.
L'immagine QEMU canonica precedente è conservata; per vedere la build di
sviluppo, dalla radice del repository:

```sh
CIUKIOS_FULL_IMG=build/full/ui-redesign-2026-09-25/development.img \
QEMU_MEMORY_MB=128 bash scripts/qemu_run_full.sh --no-build
```

Questo avvio usa normalmente la VGA standard e il modo scelto dal BIOS/EDID.
I test 2K richiedono esplicitamente una VGA virtuale da 32 MiB con geometria
preferita 2560×1440. Due pagine a 32 bit richiedono almeno 28,125 MiB di VRAM,
oltre all'eventuale padding. Una scheda senza tale capacità usa ancora il
percorso a pagina singola: non è certificato privo di aggiornamenti a fasce.

La ISO nuova non è stata masterizzata. Non sono state fatte scritture su HDD
fisici. Il limite della sessione VSCode è 8.000.000.000 byte.

## Lavoro restante

Il prossimo intervento proposto è un backend LFB coerente anche per
cursore, letture, testo e ripristino del modo video, accompagnato da misure
che identifichino il costo delle singole operazioni. Una copia veloce aggiunta
solo al compositor non sarebbe sufficiente: su hardware reale la modalità
lineare può disabilitare l'accesso a banchi. La presentazione attuale usa
display-start immediato e non garantisce sincronizzazione col retrace fisico.

Il seguito è organizzato in **tre incarichi eseguibili in parallelo**, con
proprietà dei file e prompt completi in
[ui-next-phases-2026-09-25.md](ui-next-phases-2026-09-25.md): backend video,
finestre/interazioni e collaudo indipendente. La prova su una ATI 9200 reale
rimane distinta dal superamento dei test QEMU.
