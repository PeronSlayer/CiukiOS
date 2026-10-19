# Decisione del proprietario: CiukiDOS resta il DOS di CiukiOS; le chiamate INT 21h su file passano al VMM (modello IFS Manager di Windows 9x); FreeDOS solo come riferimento di conformità e ospite di riserva

- **Data:** 2026-10-19
- **Tipo:** decisione (architettura DOS), analisi
- **Versione/commit:** 0.9.0 pre-alpha / main `e5b85ef7`
- **Stato:** in vigore; direttiva f3-48 avviata

## Contesto

Dopo la prova dell'immagine `4bcd1d43` (voce 2026-10-19-05) il proprietario ha chiesto se, per avere la stessa compatibilità di FreeDOS, si potessero prendere "pari pari" le sue chiamate INT 21h, e subito dopo ha precisato di non voler abbandonare CiukiDOS, perché non sarebbe più qualcosa di suo. Fatti rilevanti: CiukiDOS (`src/boot/floppy_stage1.asm`, 25.246 righe, 195 routine `int21_*`) implementa già le funzioni INT 21h 00h–10h, 19h–1Ch, 20h, 25h, 29h–3Ah, 3Bh–4Fh, 50h–59h, 5Dh, 60h, 62h, 66h–68h, 88h, 89h, C2h, F1h, con esecuzione di COM/MZ, PSP, SFT, FCB di base, directory, FindFirst/FindNext, attributi, IOCTL e vettori; mancano in particolare la famiglia FCB di I/O (11h–17h, 21h–24h, 26h–28h), 5Ah/5Bh (file temporaneo/nuovo), 5Ch (blocco di record), 65h (informazioni di paese estese), 6Ch (apertura estesa). L'I/O su file del guest passa oggi solo da INT 13h emulato dal VMM sull'immagine FAT16 privata (due finestre di lettura da 64 KiB; voce 2026-10-19-05 e direttiva f3-47). I problemi di disco visti con DOOM non dipendono dal kernel DOS.

## Decisioni

1. CiukiDOS resta il kernel DOS del progetto e continua a crescere; FreeDOS non viene adottato come kernel ospite.
2. La compatibilità INT 21h si ottiene come in Windows 9x: il kernel reale serve processi, PSP, memoria e vettori; le chiamate su file e directory (apertura, lettura, scrittura, posizionamento, FindFirst/FindNext, attributi, date, directory corrente, creazione/eliminazione/rinomina, duplicazione di handle, IOCTL di file, blocco di record) vengono intercettate dal VMM e servite in ring 0 dal VFS di CiukiOS con semantica documentata (MS-DOS Programmer's Reference, Ralf Brown's Interrupt List), mantenendo PSP, JFT e SFT coerenti con il guest. È il punto D2 del contratto `docs/design/win98-class-resource-management.md`, anticipato su richiesta del proprietario.
3. FreeDOS serve come riferimento di conformità: una batteria differenziale esegue gli stessi programmi di prova sotto FreeDOS e sotto CiukiDOS/VMM e confronta i risultati; per il solo sviluppo di questa batteria è ammesso QEMU (eccezione alla sospensione dei gate, voce 2026-10-19-03). FreeDOS resta inoltre disponibile come ospite di riserva selezionabile per singolo gioco, mai predefinito.
4. Programma: direttiva f3-48 (contratto dell'API DOS nel VMM, inventario delle chiamate usate dalla libreria di giochi, batteria di conformità, poi le fasi di implementazione f3-49 e seguenti definite nella stessa direttiva), avviata il 2026-10-19.

## Riferimenti

- `docs/directives/f3-48`, `docs/design/dos-dpmi-contract.md`, `docs/design/win98-class-resource-management.md` (D2); voci 2026-10-19-03, 2026-10-19-04, 2026-10-19-05.
