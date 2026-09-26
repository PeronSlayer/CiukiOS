# Tre incarichi paralleli per completare la UI

Il redesign del 25 settembre è implementato, ma il requisito di fluidità a
2560×1440 **non è soddisfatto**. La doppia pagina VBE elimina le immagini
composte a metà nelle prove QEMU; massimizzare/ripristinare richiede ancora
circa 2,4 secondi nel percorso a banchi misurato. Il costo delle singole
operazioni deve essere profilato prima di attribuire tutta la latenza a una causa.
Il supporto e le prestazioni su una ATI 9200 fisica non sono stati misurati.

I tre prompt qui sotto si possono assegnare contemporaneamente. Ogni incarico
ha file distinti; il terzo prepara i controlli mentre gli altri implementano,
poi esegue l'integrazione sulle stesse immagini identificate da SHA-256.
Leggere anche `docs/ui-redesign-2026-09-25.md`, i tre rapporti collegati e
`AGENTS.md`. Non ripartire dal vecchio desktop né cancellare il lavoro presente.

## Prompt 1 — Backend video e prestazioni

```text
Lavora nel repository /home/peronslayer/Desktop/CiukiOS. Devi completare il
backend video della nuova UI per Pentium III, 128 MB di RAM e schede dell'epoca
come ATI Radeon 9200, con pixel nativi fino a 2560x1440. Il lavoro NON è finito:
la doppia pagina VBE è corretta nelle catture QEMU ma il percorso bancato
impiega circa 2,4 s per massimizzare/ripristinare in 2K.

Leggi AGENTS.md e docs/ui-rendering-2026-09-25.md; usa Semble prima dei file
grandi. Parti dai sorgenti correnti e dalle prove negative conservate.
Possiedi src/com/vbe_console.inc, vbe_modes.inc, vbe_text_batch.inc,
shell_gui_compositor.inc ed eventuali nuovi file video dedicati. Coordina i
soli punti di chiamata nella shell con l'incarico 2; non modificare kernel,
filesystem, audio o launcher dei giochi.

Progetta e implementa un backend framebuffer lineare coerente. Non basta
accelerare una copia: lettura/ripristino del cursore, pixel, span, testo,
clear, pitch e cambio pagina devono usare lo stesso backend, anche quando
il BIOS disabilita l'apertura a banchi in modalità LFB. Valida PhysBasePtr,
dimensioni, maschere colore e memoria; gestisci errore del BIOS e ritorno a
DOS. Se usi transizioni protette, prova conservazione di CR0/GDTR/segmenti,
stato A20 e interrupt, tempi limitati e rifiuto sicuro del contesto V86.
Non introdurre hook residenti né lasciare segmenti unreal agli eseguibili DOS.

Mantieni le API vc_* e ui_comp_*, ui_front_base e ui_comp_recover compatibili
oppure concorda prima una modifica minima. Nessuna riduzione silenziosa di
risoluzione/profondità per ottenere un risultato di benchmark migliore.
Misura latenza di input, drag e massimizzazione e distingui KVM, TCG e
hardware reale. Il controllo mouse dopo Doom fallisce a 300 ms ma riceve lo
stesso pacchetto dopo circa 777 ms: correggi la latenza, non la soglia del test.
Estendi test_ui_rendering.py con istruzioni assemblate e
guardie di memoria; consegna codice, ABI, risultati positivi/negativi e limiti
ancora aperti. Non dichiarare supporto ATI fisico senza provarlo.
```

## Prompt 2 — Finestre, interazioni e animazioni

```text
Lavora nel repository /home/peronslayer/Desktop/CiukiOS. Completa le
interazioni della UI già ridisegnata in stile pre-2000: platino, indaco,
desktop verde petrolio, font bitmap nativi. Mantieni inglese, foto di Ciuki
all'avvio e la sola tagline "A modern Retro OS". Non rifare il tema da zero.
CiukiOS è dedicato a Ciuki: mantieni il logo con il suo volto stilizzato,
documentato in assets/brand/README.md e AGENTS.md; non ripristinare la lettera C.

Leggi AGENTS.md, docs/ui-design-2026-09-25.md e ui-redesign-2026-09-25.md.
Possiedi shell_gui.inc, shell_gui_input.inc, shell_gui_windows.inc,
shell_gui_draw.inc, shell_gui_detail.inc, shell_gui_dialogs.inc, ui_theme.inc
e la sola presentazione di setup_graphics.inc. Non modificare il backend
vc_*/compositor, kernel, disco, audio o launcher: concorda i punti di
integrazione con l'incarico 1.

Preserva le correzioni già provate: focus distinto per finestra, selezione
senza vecchi riquadri residui, cancellazione del click rilasciato su un altro
controllo, limiti del cursore, finestre sovrapposte e contenuto Run conservato.
Completa una revisione di TUTTI i controlli da mouse e tastiera, inclusi
schermate 800x600, finestre massimizzate, taskbar affollata, dialoghi
transitori, cambi video annullati e ritorno da DOS/Windows/giochi.
Prova la digitazione rapida durante il paint eseguendo il comando intero e
confrontando l'output: i soli pixel del testo conservato non sono sufficienti.

Le animazioni devono restare brevi, interrompibili e locali agli elementi;
non usare attese attive, I/O su disco durante il paint, ridisegni integrali
per hover/focus o buffer proporzionali alla RAM del PC. Il cambio video deve
avere anteprima e timeout; le alte risoluzioni del desktop non devono
alterare il driver Windows 3.1. Non fingere multitasking per applicazioni DOS.

Collabora con l'incarico 3 su sequenze riproducibili e screenshot effettivi.
Consegna solo cambiamenti con comportamento verificato, una mappa completa
dei controlli e un elenco esplicito di difetti/rischi rimanenti. Le sole
schermate statiche non dimostrano fluidità né assenza di glitch.
```

## Prompt 3 — Collaudo indipendente e integrazione

```text
Lavora nel repository /home/peronslayer/Desktop/CiukiOS come responsabile
indipendente del collaudo. Leggi AGENTS.md, docs/ui-validation-2026-09-25.md,
ui-redesign-2026-09-25.md e le evidenze in build/full/ui-redesign-2026-09-25.
Possiedi scripts/qemu_test_*.py relativi a questa UI, fixture di test, report
e manifest; non correggere produzione ASM mentre gli incarichi 1 e 2 la
modificano. Puoi preparare tutti i test in parallelo e integrarli alla fine.

Usa copie delle immagini e almeno Pentium III/128 MB a 800x600, 1024x768,
1920x1080 e 2560x1440. Prova framebuffer con pitch non banale, più profondità,
poca VRAM, BIOS senza pagine multiple e rifiuti di modalità. Verifica pixel
prima/durante/dopo apertura, overlap, trascinamento, ridimensionamento,
minimizzazione/ripristino e cursore ai quattro bordi. Dopo un cambio pagina
ogni immagine deve essere completa; conserva i fotogrammi intermedi.

Non usare solo marker seriali come prova di successo. Misura anche latenza,
attività CPU a riposo, input durante animazioni e RAM; non attribuire a un
Pentium III fisico le prestazioni della CPU host con KVM. Ripeti le tre
regressioni negative di selezione/focus/rilascio su finestra diversa.
Verifica boot CD e HDD a 128 MB, Setup annullamento/installazione/riavvio su
dischi virtuali sacrificabili, directory complete, COMMAND.COM, Windows 3.1
(ridimensionamento, WAV/MIDI, uscita), Doom classico, Doom Vanille e Wolf3D
(partita, input, audio e uscita), tornando alla UI nella stessa sessione.

Conserva le prove di latenza fallite dopo Doom e nel primo paint della modalità
SAFE: il successivo completamento corretto non trasforma quei controlli in PASS.
Prepara raccolta di modalità VBE, VRAM, BIOS e tempi su hardware reale ATI 9200
o equivalente; se non hai l'hardware, indica chiaramente il requisito non
verificato. Non allungare attese né indebolire controlli per far passare una
regressione. Una correzione del test richiede prova dell'errore del test e una
nuova esecuzione, conservando quella fallita. Consegna immagini e hash esatti,
comandi, risultati, screenshot e problemi aperti. Non pubblicare/masterizzare
una build come completa finché qualità visiva e fluidità richieste non sono
entrambe dimostrate.
```
