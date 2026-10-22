# 2026-10-22-08 — Decisione: compositore sul modello Windows 98/X11 con regioni X11 e pixman

## Decisione

Il proprietario, il 2026-10-22 verso le 18:30, ha chiesto se convenisse
«copiare» il compositore di Linux o di Windows 98/ME. Valutazione del lead:
Windows 98/ME non ha un compositore (disegno diretto clippato per regioni,
BitBlt accelerato dal driver) e non è open source; X.Org, Wayland/Weston,
DirectFB e Microwindows non sono integrabili così come sono (dimensioni,
dipendenze POSIX/KMS, riscrittura delle applicazioni, maturità). Il
proprietario ha approvato la raccomandazione: rifare il compositore del
desktop sul modello Windows 98/X11 (aggiornamenti per regione, mai frame
interi) riusando sotto licenza MIT il codice delle regioni di X11/pixman e i
percorsi veloci di pixman, senza cambiare le applicazioni. Attuazione nella
direttiva f2-95 (emendamento 1).

## Motivazione tecnica

Dalla prova a4d9454e (2026-10-22-07): con il solo lettore musicale aperto il
desktop componeva circa 40 frame interi al secondo e usava il 40–50 % della
CPU del Pentium III, affamando lettore, browser e il proprio loop di input.
Il motore 2D (usato, `used=1`) non riduce il lavoro finché la composizione
resta a frame interi.
