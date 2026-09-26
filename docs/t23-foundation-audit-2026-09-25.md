# T23: revisione dei percorsi di base — 25 settembre 2026

Il desktop del CD `90dae91c…` funziona sul T23 quando si esclude il suono
iniziale; `SOUND /D` provoca invece un riavvio. Il risultato di COMDEMO non
è conclusivo: il rapido ritorno alla shell comprende un cambio video e non
prova da solo il fallimento di EXEC. La causa fisica del riavvio non è ancora
accertata.

Su richiesta del proprietario, questa revisione comprende lettura del codice,
correzioni e compilazione, **senza ulteriori esecuzioni QEMU o test CPU**.
I risultati PASS delle immagini precedenti non qualificano queste modifiche.
Non è una dichiarazione di compatibilità completa del T23.

## Kernel e avvio dei programmi

- Il salvataggio del DTA del processo padre caricava il segmento in AX prima
  di una funzione che restituiva il PSP nello stesso registro. Ora recupera
  prima il PSP e poi il vero segmento DTA: EXEC non salva più il PSP al posto
  del segmento del buffer dei servizi DOS.
- L'allocatore rispetta sia la memoria convenzionale indicata dal BIOS sia
  l'inizio valido dell'EBDA. Un limite inferiore all'arena non produce una
  dimensione negativa interpretata come una grande allocazione.
- L'output video del kernel conserva i registri del chiamante; l'attesa del
  trasmettitore seriale ha un limite, quindi una COM1 non pronta non blocca
  indefinitamente la stampa.
- I launcher Doom, Doom Vanille, Wolf3D e Windows 3.1 passano strutture FCB
  complete da 16 byte a EXEC, invece di oggetti da quattro byte seguiti da
  altri dati. Distinguono i risultati HDPMI documentati: 0–2 installazione
  riuscita, 3 host già presente, valori superiori errore. Scaricano l'host
  solo quando lo hanno installato e controllano l'esito dello scaricamento.

## Firmware, disco e video

- Il settore di avvio normalizza CS e DF, tenta EDD prima del fallback CHS
  e conserva registri e segmenti attraverso INT 13h. I fallback CHS fissi
  sono limitati ai layout per cui sono definiti; non leggono un settore
  diverso dopo un errore. L'MBR controlla la firma del settore caricato.
- Stage1 conserva anche i registri estesi e i segmenti usati dal chiamante,
  ripristina il numero di settori della richiesta EDD e limita le attese
  seriali. L'immediato modificato dal setup per la lettera disco rimane
  all'offset 23.
- La shell protegge il proprio stato attorno alle chiamate INT 10h e prende
  esplicitamente i risultati richiesti per pagina, cursore e puntatore font.
  Il recupero del font VBE segue lo stesso contratto. Queste modifiche non
  disabilitano il desktop ad alta risoluzione o il framebuffer lineare.

## Setup e scrittura del disco

- Le chiamate BIOS disco, timer e PCI conservano segmenti e registri del
  chiamante; restituiscono solo i risultati previsti. I rami di errore PCI
  non usano più DS prima del ripristino.
- La suddivisione CHS usa divisioni a 32 bit e rifiuta cilindri oltre 1023
  e geometrie non valide. Non può più generare un'eccezione di divisione
  quando il quoziente di una divisione a 16 bit sarebbe troppo grande.
- Le letture BIOS che attraversano un confine fisico di 64 KiB passano per
  un buffer privato allineato a 512 byte; viene copiato solo un settore letto
  con successo. Le richieste normali mantengono i trasferimenti a blocchi.
  Anche le due letture isolate di metadati usano il buffer allineato.
- Il percorso di copia file controlla gli errori di chiusura; la sonda disco
  reimposta il numero di settori EDD invece di riutilizzare un valore lasciato
  da una richiesta precedente.
- La scrittura ATA PIO, il confronto CRC dopo FLUSH e la pubblicazione finale
  dell'MBR restano nel percorso esistente. Questa revisione non dimostra che
  il contenuto di un HDD già installato sia integro e non lo ripara a distanza.

## Audio e proprietà della memoria DMA

- BOOTSND/SOUND e AC97INIT controllano tutti e tre i canali ICH (PCM in,
  PCM out, microfono) prima di abilitare il bus mastering globale. Un canale
  preesistente attivo impedisce la presa di possesso.
- Il rilascio arresta l'uscita posseduta, attende DCH e il reset del canale
  prima di staccare la lista descrittori e liberare il PCM. Letture tutte a
  uno non vengono accettate come conferma di hardware pronto o DMA fermo.
- Se l'arresto scade, è necessaria una rilettura PCI che confermi BME=0.
  In assenza di questa conferma il programma mantiene memoria e descrittori
  allocati, mostra un errore e resta fermo senza riavvio automatico. Uscire
  normalmente da DOS in quel caso rilascerebbe memoria ancora esposta al DMA.
- AC97INIT ha attese finite e richiede il completamento dell'ultimo buffer;
  entrambi i player impostano esplicitamente l'uscita stereo.
- `SOUND /D` mantiene visibile il primo messaggio prima di allocazioni e
  accessi PCI: Invio continua, Esc ritorna alla shell. Questo cambiamento
  diagnostico non costituisce una prova della correzione del riavvio fisico.

Riferimenti del protocollo: [Intel ICH3, sezioni 13.1.3 e 13.2](https://www.intel.com/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf)
e [driver Linux intel8x0](https://github.com/torvalds/linux/blob/v6.12/sound/pci/intel8x0.c).
I driver T23 ufficiali e i componenti DOS compilati sono documentati
[separatamente](t23-drivers-2026-09-25.md); ricompilarli non correggeva questi
difetti nel codice di CiukiOS.

## Artefatti e limiti

Directory del nuovo candidato: `build/full/t23-foundation-2026-09-25/`.
Le immagini precedenti sono conservate. Nessuna nuova masterizzazione è
stata eseguita durante questa revisione. Il limite RAM della sessione VS Code
resta 8.000.000.000 byte.

La build completa HDD e CD è terminata correttamente. I 12 binari elencati
in `packaged-binaries.json`, estratti da ciascuna delle due immagini FAT,
corrispondono byte per byte agli oggetti della rispettiva compilazione.
I 30 sorgenti/script censiti in `source-freeze.json` sono rimasti invariati
durante la build. Questo controllo riguarda il confezionamento, non esegue
i programmi.

ISO: `cd/CiukiOS_full_cd_0-7-1.iso`, **149.399.552 byte**.
SHA-256: `4605cd6f2f4f3d9d8b02d57398229a2228fffb6f145362644a14b4651ad31ec0`.

| Componente CD | Byte |
|---|---:|
| Boot / Stage1 | 512 / 1.588 |
| Kernel CIUKIDOS.SYS | 43.239 |
| SHELL.COM / COMMAND.COM | 59.488 / 14.704 |
| SETUP.COM | 58.048 |
| VGASETUP.COM | 20.832 |
| BOOTSND.COM e SOUND.COM | 9.136 ciascuno |
| AC97INIT.COM | 15.120 |

Le dimensioni rispettano i limiti della build; il kernel ha solo 25 byte
di margine rispetto al suo limite attuale di 0xA900. Non aggiungere codice
senza ricontrollare il layout. Il setup della build completa differisce
dalle compilazioni isolate per le costanti calcolate dal processo di build.

Log: `build.log`, `build-cd.log`; riepilogo: `release-summary.json`.
`canonical-restored.json` registra il ripristino identico dei precedenti
artefatti CD canonici. La nuova ISO è esclusivamente nella directory del
candidato indicata sopra. **Nessun avvio di questa build è stato eseguito,
né in emulazione né sul T23.**

## Masterizzazione — 26 settembre 2026

Su richiesta del proprietario, questa stessa ISO (`4605cd6f…`) è stata
masterizzata sul CD-RW in `/dev/sr0`, con cancellazione rapida e modalità DAO.
Completamento alle 08:23:01 UTC (10:23:01 ora italiana), codice di uscita 0:
149.399.552 byte scritti, 72.949 settori. Il drive ha usato circa 10× a fronte
della richiesta di 4×. Espulsione richiesta con `-eject`; nessuna rilettura
completa del supporto. Log e metadati sono in `burn-cdrw.json` e nella
directory `burn-20260926T082023Z/` del candidato. La masterizzazione non
modifica lo stato della verifica funzionale sul T23, ancora da confermare.
