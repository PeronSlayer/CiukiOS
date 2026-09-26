# T23: secondo riscontro, audio e compatibilità DOS

## Evidenza fornita dall'utente

Prove da HDD sul T23, successive alla ISO con SHA-256
`1849711fa682576ae827ac78e2a3aedfb28f3f3c1696bc34c3988e362e4ae012`:

- Doom e DoomVan: funzionamento e audio confermati dall'utente.
- Wolf3D: uscita corretta, ma audio nuovamente assente.
- Windows: foto dopo l'uscita con ritorno alla shell; non è una conferma
  di audio udibile. Il precedente riscontro segnalava riproduzione silenziosa.
- DOS Navigator: avvio lento, utilizzo possibile, crash alla chiusura.
- Costa: non si apre; comando e sintomo dettagliato non ancora disponibili.

Foto Windows: ICH3, IRQ 11, 44100 Hz, IRQ=8610, PCM=704, OPL=6332,
OUT=2638, mixer virtuale master/voice/midi=CC.
Foto Wolf3D: stesso controller/frequenza, IRQ=5356, PCM=0, OPL=4224,
OUT=3744, mixer virtuale CC. OUT conta campioni non nulli prima della
scrittura al controller: **non verifica DAC, amplificatore o altoparlanti**.

## Modifica implementata

`patches/vsbhda-ciukios-ich-codec.patch`, applicata dal builder al sorgente
VSBHDA 2.0 fissato a `75fa4bbfea70cbcc0c40d1212f04952ff8abbf16`:

- distingue disponibilità AC-link da disponibilità di DAC/mixer/riferimento
  analogico; risveglia il percorso di riproduzione e attende i bit di stato,
  con limite temporale ed errore se il codec resta indisponibile;
- conserva EAPD (polarità dipendente dalla scheda) e lo stato ADC;
- non procede con letture/scritture se il semaforo del codec scade; riconosce
  e azzera correttamente lo stato di timeout RCS invece di accettare la lettura;
- applica il modo analogico CS4299 solo a ID `4352:593x` (maschera `FFF8`);
  non abilita né programma registri S/PDIF standard non dichiarati dal codec;
- stampa dopo lo stop i registri **fisici** di alimentazione, volumi e
  frequenza DAC, più il numero di errori di accesso. La stampa non avviene
  nell'interrupt e non sostituisce una misura dell'uscita analogica.

La modifica è condivisa dai driver a 32 bit e dal modulo SNDCARD.DRV del
percorso a 16 bit. Non modifica i motori Doom/Wolf4GW, i driver Windows,
gli IRQ scelti dai launcher, o le API DOS in base al nome del programma.

L'assenza del controllo analogico è verificata nel codice precedente e la
sequenza è motivata dalle specifiche; **non è ancora dimostrato che fosse
la causa del silenzio riferito sul T23**.

## Verifiche e limiti

`python3 scripts/test_vsbhda_ich_codec.py` compila le funzioni di produzione
con un modello dei registri. Verifica codec standard, risveglio dopo warm
reset, preservazione EAPD/ADC, quirk CS4299, timeout precedenti e nuovi,
semaforo occupato, AC-link assente, analogico indisponibile e ID assente.
Non simula il T23, il suo DMA o l'amplificatore.

Il test Windows ha una nuova opzione `WINDOWS31_SMOKE_DOS_HANDOFF=1`:
Windows -> uscita -> DOS Navigator -> Alt+X -> sonda runtime DOS ->
secondo Windows -> uscita -> Costa/Calculator, senza riavvio intermedio.
Questo amplia la copertura del recupero di stato fra programmi differenti.
I controlli QEMU restano distinti dalla verifica dell'hardware reale.

I log della ripresa sono conservati sotto
`build/full/t23-followup-2026-09-05/`, non solo in `/tmp`.

Risultati della ripresa, sul driver finale ricompilato:

- modello dei registri: tutti i nove casi completati;
- patch: applicazione verificata anche sul file del commit upstream non
  modificato, oltre al controllo inverso nel checkout usato per compilare;
- QEMU Windows con `WINDOWS31_SMOKE_DOS_HANDOFF=1`: PASS; desktop Windows,
  riproduzione WAV/MIDI, chiusura, DOS Navigator/Alt+X, sonda runtime, secondo
  Windows, ritorno a DOS e avvio della Calculator di Costa. La schermata
  `windows/costa-calculator.png` è stata ispezionata, non solo classificata
  come "grafica non vuota";
- QEMU Doom: PASS, waveform AC-RMS 415,66 e ritorno alla shell;
- QEMU DoomVan: PASS, waveform AC-RMS 829,99 e ritorno alla shell;
- QEMU Wolf3D: PASS, waveform AC-RMS 483,21, uscita con F10/Y e successivo
  comando nella shell.

La schermata Windows di uscita mostra i nuovi registri fisici:
`AC97=8384:7600 POWER=000f EXT=0009 DAC=44100`, volumi fisici zero
(nessuna attenuazione/mute) e `IOERR=0`. È il codec simulato di QEMU,
**non il codec del T23**. La diagnostica nel modulo Windows appare sul video
ma non tutte le sue righe passano nel log seriale: conservare la schermata.

Non sono state ripetute in questa ripresa le prove di installazione HDD,
reboot o VGASETUP. Non vengono dichiarate risolte sulla base dei test audio.

Anche `qemu_test_full_cd_shell_com_boot.sh --no-build` è PASS sulla nuova
ISO: avvio Live CD, shell D:, lettura ad alto LBA, COM/MZ, sonda runtime,
EXEC annidato e servizi della shell. Non è una prova di clonazione su HDD.

DOS Navigator e Costa restano problemi aperti su hardware reale. Mancano il
comando esatto, lo stato dello schermo al guasto e la distinzione fra avvio
a freddo e avvio dopo altri programmi. Non è stata introdotta una correzione
speculativa nel kernel né dichiarata compatibilità completa con tutto DOS.

## Nuova immagine candidata

`build/full/CiukiOS_full_cd_0-7-1.iso`, 101261312 byte.

SHA-256: `f4aee9f0447625fca73f7a8c36de2682282c342bd53d72a345da0d216504b904`.

Estratta l'immagine disco dalla ISO e controllata la partizione FAT16 a LBA 63:

- `SBEMU/VSBHDA.EXE`: SHA-256
  `c2640ec2c543c2dc247b91df71b5a76761e755d6efd93c15817434c04c26590b`;
- `SBEMU/SNDCARD.DRV` e `WINDOWS/SNDCARD.DRV`: SHA-256
  `6ec828c14faeffaaeae65cdc34413fc01185c1d734012b68be8ee3e80b593bf6`.

Coincidono con i prodotti della ricompilazione. I percorsi effettivi del driver
sono `C:\SBEMU` e `C:\WINDOWS`, non `C:\SYSTEM\DRIVERS\SBEMU`.

La ISO precedentemente provata dall'utente è conservata in
`build/full/t23-followup-2026-09-05/CiukiOS_full_cd_0-7-1_before-codec.iso`,
con SHA-256 verificato uguale a `1849711f…e012` riportato sopra.
**Il CD fisico non è stato rimasterizzato in questa ripresa.** Questa è una
build candidata alla verifica sul T23, non una release dichiarata priva dei
problemi segnalati.

## Fonti primarie

- [Intel AC'97 2.1, sezioni 6.3.11 e 7](https://www.alsa-project.org/files/pub/datasheets/intel/ac97r21.pdf):
  stato AC-link distinto dallo stato analogico e gestione dell'alimentazione.
- [Cirrus Logic CS4299, registri e reset](https://www.mouser.com/datasheet/2/76/CS4299_F1-611188.pdf).
- [Linux ALSA: patch Cirrus CS4299](https://github.com/torvalds/linux/blob/master/sound/pci/ac97/ac97_patch.c):
  modo AC e registri S/PDIF specifici Cirrus.
- [VSBHDA upstream](https://github.com/Baron-von-Riedesel/VSBHDA).

## Limite RAM della workstation durante la ripresa

Su richiesta esplicita è stato configurato il cgroup della sessione corrente
VS Code `app-code-3621.scope`: `MemoryMax=6000000000`, `MemoryHigh=5000000000`,
con `systemctl --user set-property --runtime`. Il kernel arrotonda per difetto
ai confini delle pagine. Il limite comprende i processi figli della sessione;
non è un limite globale della macchina e non è persistente al prossimo avvio
di VS Code. La swap non è disabilitata. Le prove della ripresa vengono eseguite
con una sola VM alla volta.
