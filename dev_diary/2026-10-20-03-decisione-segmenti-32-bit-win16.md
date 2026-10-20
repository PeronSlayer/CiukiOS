# Decisione del proprietario: i programmi Win16 con segmenti a 32 bit entrano nel perimetro di F5

- **Data:** 2026-10-20
- **Tipo:** decisione del proprietario (perimetro)
- **Versione/commit:** 0.9.2 pre-alpha / main (commit di questa voce)
- **Stato:** registrata; implementazione pianificata dopo il nucleo Win16 a 16 bit

## Fatti

1. Le disposizioni P0 di f5-19P (fuse il 2026-10-20, `docs/design/win16-subsystem-contract.md`, sezione P) limitavano la prima fase Win16 ai descrittori a 16 bit e rifiutavano per nome i segmenti a 32 bit (riga D-C01 della tabella P8).
2. Alla domanda del lead se i programmi Win16 con segmenti a 32 bit debbano mai entrare nel perimetro, il proprietario ha risposto: "si vanno inclusi".

## Decisioni

1. Entrano nel perimetro di F5: i segmenti di codice e dati a 32 bit (USE32) nei programmi e nelle DLL NE, i programmi costruiti con l'estensore Win386 di Watcom, l'API di `WINMEM32.DLL` e i ponti tra codice a 32 e a 16 bit usati dai programmi dell'epoca (Universal Thunk di Win32s, thunk piatti di Windows 95/98 generati dal thunk compiler).
2. Il nucleo a 16 bit (f5-19b, f5-19c, f5-19d) procede come pianificato; il modello C01 (LDT per host, chiamata di esecuzione 110h) resta valido e viene esteso ai descrittori a 32 bit in una tappa dedicata, f5-19e, scritta e rivista prima del codice, con fonti primarie (Windows 3.1/95/98 SDK e DDK, documentazione Watcom Win386, documentazione Microsoft di Win32s e del thunk compiler).
3. Fino a f5-19e quei programmi restano rifiutati per nome, con il record di rifiuto esistente.

## Riferimenti

- `docs/directives/f5-19p-win16-p0-dispositions.md` (revisione del lead, punto 4); `docs/design/win16-subsystem-contract.md`, tabella P8 riga D-C01; voce 2026-10-20-02.
