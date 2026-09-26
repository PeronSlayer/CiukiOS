# IBM T23: audio, uscita giochi e VGASETUP — 2026-09-05

Questo documento registra il primo intervento e la relativa masterizzazione.
Per il successivo riscontro del T23 (Doom funzionante, Wolf3D silenzioso,
DOS Navigator/Costa problematici) e la nuova build candidata vedere
[il secondo intervento](hardware-followup-audio-dos-2026-09-05.md).

## Risultati riferiti dall'utente (HDD reale)

- Wolf3D: audio corretto; blocco sulla conferma di uscita anche con Y/Invio.
- Doom/DoomVan: niente audio, avviati con `run doom.exe` nelle rispettive cartelle.
- Windows 3.1: `run win`, riproduzione senza crash ma silenziosa; uscita alla shell corretta.
- VGASETUP: il comando ridisegna soltanto la shell.

Non è stato possibile eseguire verifiche sul T23 da questa workstation.

## Correzioni

### Wolf4GW

`Keyboard[]` viene scritto dall'IRQ1 ma non era `volatile`. Nel binario precedente
la conferma di uscita caricava Y una sola volta, poi eseguiva:

```asm
mov al, Keyboard[0x15]
loop: test al, al
      jne loop
```

Il rilascio del tasto non poteva sbloccare quel ciclo. Dichiarazione, definizione
e azzeramento dell'array ora conservano l'accesso volatile. La disassemblazione
della ricompilazione mostra `cmp byte ptr Keyboard+0x15,0` dentro il ciclo.
Invio resta un'alternativa a Y, non la soluzione al difetto.

### Doom e DoomVan

Gli eseguibili pubblici ora sono launcher MZ autentici, compilati dallo stesso
corpo dei launcher COM. Conservano PSP e coda comando, riducono il proprio blocco
di memoria e avviano il driver PCI prima del motore. Non c'è un reindirizzamento
globale della shell né una copia COM rinominata EXE.

- `DOOM/DOOM.EXE` e `DOOM.COM` avviano il motore `DOOMCORE.EXE`.
- `DOOMVAN/DOOM.EXE`, `DOOMVAN.EXE`, `PCDOOM.EXE` e `DOOMVAN.COM`
  avviano `DOS4GW.EXE PCDMCORE.EXE`.
- I test specifici del motore senza virtualizzazione usano il nome del core.
- Il ritorno non nullo del caricamento residente HDPMI non è trattato come
  fallimento di EXEC; i fallimenti del figlio VSBHDA sono invece propagati.

### VGASETUP e shell

La shell azzerava la modalità video dopo ogni comando, cancellandone subito
l'output. Ora conserva il contenuto della normale pagina testo 80 colonne,
ripristinando font e cursore; continua a ripristinare una modalità testo pulita
quando il figlio lascia grafica, pagine alternative o modalità incompatibili.
VGASETUP senza argomenti stampa stato e comandi disponibili.

Il test video controlla ora anche B800:0000 dopo il ritorno al prompt: la sola
presenza del testo sul log seriale non dimostrava che fosse rimasto sullo schermo.

### Windows: problema aperto, diagnostica aggiunta

Non sono stati cambiati IRQ, frequenza, driver Windows o modalità Standard sulla
base di ipotesi. VSBHDA/VSBHDA16 ora stampano, dopo la chiusura del figlio:

- scheda, IRQ hardware, frequenza effettiva e maschera PIC;
- interrupt audio serviti (`IRQ`), blocchi PCM e OPL;
- blocchi non silenziosi dopo il mixer (`OUT`) e registri volume virtuali.

I contatori non chiamano DOS/BIOS dall'interrupt. La stampa avviene dopo lo stop
del controller, in una pagina testo ripulita. `OUT > 0` dimostra un segnale
digitale prima della scrittura al controller, **non** suono dagli altoparlanti.

Per il T23 occorre riprodurre un WAV e CANYON, uscire da Windows e fotografare
le righe `AUDIO:`. Questo è il prossimo dato necessario; il bug non è chiuso.

## Verifiche locali

- Compilazione NASM dei launcher COM/MZ, shell e VGASETUP: riuscita.
- Ricompilazione Wolf4GW e VSBHDA/VSBHDA16 dai sorgenti fissati: riuscita.
- QEMU: VGASETUP visibile nella memoria video, persistenza 80x50 e ripristino
  profilo Windows VGA: superati.
- QEMU: Wolf3D, uscita con F10/Y, cleanup e comando nella shell: superati.
- QEMU: Doom.EXE e PCDOOM.EXE, gameplay, waveform AC97 e uscita: superati.
- QEMU: Windows, WAV/MIDI, chiusura finestra, uscita e secondo avvio: superati.

Questi risultati non sono una verifica dell'audio o dell'uscita sul T23.
Non sono stati ripetuti i test di installazione HDD in questa sessione.

La pulizia finale dello schermo nel driver è stata ricompilata e ricontrollata
con Wolf3D e Windows. Lo screenshot di uscita Wolf3D mostra resoconto e prompt
leggibili, senza i residui della schermata del gioco.

Esempi di contatori misurati in QEMU (non sono letture del T23):

- Doom, avvio `.EXE`: `IRQ=4606 PCM=2709 OPL=2719 OUT=2489`.
- DoomVan, avvio `.EXE`: `IRQ=4526 PCM=2565 OPL=2440 OUT=2278`.
- Wolf3D, controllo finale: `IRQ=5130 PCM=0 OPL=4831 OUT=4778`.
- Windows, controllo finale: `IRQ=7359 PCM=332 OPL=7242 OUT=2154`.

Il controllo Wolf3D in questa scena dimostra il segnale OPL e l'uscita;
`PCM=0` non dimostra effetti digitalizzati in quella scena.

Artefatti Windows finali conservati in `/tmp/ciukios-windows31-smoke.3ysgAt`;
log e waveform dei giochi in `build/full/qemu-full-*-audio.*`.

## Immagine distribuita

`build/full/CiukiOS_full_cd_0-7-1.iso`, 101261312 byte.

SHA-256: `1849711fa682576ae827ac78e2a3aedfb28f3f3c1696bc34c3988e362e4ae012`.

CD-RW `/dev/sr0` rimasterizzato con xorriso in DAO. Rilettura diretta di tutti
i 101261312 byte dal supporto (`dd`, `iflag=direct,count_bytes`, blocchi 1 MiB):
SHA-256 identico alla ISO; comando completato con codice 0.
Questo verifica il contenuto del CD nella workstation, non il suo avvio sul T23.

Il CD precedente è conservato in
`build/full/cd-backup.cPmDDa/CiukiOS_full_cd_0-7-1_previous.iso`;
SHA-256 verificato:
`bb5d480303cf3abc4cc4788be1802fbf2a0e7d47c1a831d47de96ef43f5b9216`.

## Sorgenti consultati

- [Wolf4GW](https://github.com/TobiasKarnat/Wolf4GW), commit
  `a49ead44cb4a6476255e355c4c5e3e48bb7f1d55`.
- [VSBHDA, documentazione del progetto](https://github.com/Baron-von-Riedesel/VSBHDA/blob/main/vsbhda.txt),
  in particolare §4.5 (IRQ) e §4.6 (Windows 3.1 Standard Mode).
- VSBHDA 2.0, commit `75fa4bbfea70cbcc0c40d1212f04952ff8abbf16`;
  HDPMI 3.24, commit `f2276db9accfc57facf2588bc016a27130597bb1`.
