# Notte verso la 0.9.0: shell del desktop, icone Tango, avvio pulito, finestra DOS, DPMI, audio VM, DOOM; recupero degli accessori della 0.8

- **Data:** 2026-10-12 (notte e mattina)
- **Tipo:** modifica (fusioni, correzioni) e analisi di regressione; decisioni
- **Versione/commit:** ramo `wt/integ2` (`cc4e1301`); immagini `f51e5df7…`, `d70cf673…`, `138a457a…`, `97f889aa…`
- **Stato:** in corso (integrazione del tema grafico; campagna finale da eseguire)

## Contesto

Mandato del proprietario per la mattina: immagine utilizzabile su QEMU e sul
T23, set di icone completo e libero, sfondi, funzioni della vecchia linea
0.8, interfaccia nuova nello spirito di Mac OS 9 e Windows 98/ME, avvio
senza schermate di debug. Il lavoro è stato distribuito a agenti Claude in
worktree separati; il lead ha fuso, qualificato su QEMU e corretto.

## Cosa cambia

1. Avvio normale pulito: il loader mostra solo una riga di aiuto per 3 s, il
   kernel disegna la fotografia originale di Ciuki e tiene la console spenta
   fino al desktop (log su seriale e disk log; console riaccesa su panic,
   perdita del desktop o watchdog di 120 s). Syscall 73 `ciuki_power`
   (riavvio; spegnimento ACPI S5 letto da FADT/DSDT; solo il desktop fidato).
2. Shell del desktop: menu Ciuki con Programmi (e sottomenu Accessori e
   Sviluppo), Documenti recenti, Impostazioni, Aiuto, Esegui, Arresta;
   barra con pulsanti, vassoio con orologio e volume; icone sul desktop con
   menu contestuali; sfondi (sei procedurali, PNG/JPEG/BMP da
   `/system/wallpapers` tramite stb_image 2.30; le bitmap di Windows ME
   restano locali, mai nell'immagine); About; Pannello di controllo
   (`/bin/control`: schermo, mouse, tastiera con layout italiano, suoni,
   data e ora, sistema) con `desktop.cfg`; Esegui, Alt+Tab, Alt+F4.
3. Icone: l'intero Tango 0.8.90 in un unico store compresso nell'immagine
   (`/system/icons/tango.cis`, 246 nomi a 16/22/32/48 px, hash fissati),
   API `du_icon(nome, dimensione)` e mappa di 353 oggetti.
4. Accessori recuperati dalla 0.8 (inventario in
   `docs/design/legacy-0.8-feature-inventory.md`): Calcolatrice, Orologio e
   calendario, Visualizzatore immagini, Guida con la dedica, Controllo
   volume, Mappa caratteri; documenti recenti; tabelle tastiera US/IT.
5. Finestra DOS: la shell CIUKIDOS bloccata in INT 16h non riceveva i tasti
   accodati nell'8042 (ora il servizio riesegue con IF attivo); la ricerca
   del disco di gioco chiedeva all'heap kernel 20 KiB che non può servire
   (ENOMEM alla creazione di ogni VM da finestra e alla fase fixture di
   DOOM/Wolf3D). Il modello VGA segue la temporizzazione verticale
   programmata (70/60 Hz) nel registro 3DAh: Wolf3D attendeva il retrace.
6. DPMI: sotto QEMU TCG il client sostituisce il test del limite di segmento
   dati (TCG non lo controlla) con un #GP equivalente; su hardware resta il
   controllo reale. Dump schermo sanificato condiviso dalle sonde F3.
7. Audio delle VM: con una sorgente VM collegata la coda di silenzio non
   supera 8 periodi e il suo esaurimento ferma il trasporto senza spegnere
   il backend; DOOM legge il verdetto del timedemo da una copia dello
   schermo testo scattata alla riga di verdetto.
8. Runner: grazia di 5 s prima di contare un QEMU superstite; fixture
   ottica come `ide-cd` esplicito; attesa risposta QMP fino a 10 s; log QMP
   senza ripetizioni identiche di `query-status`; tipo di caso
   `normal-boot` in `f0-smoke`.

## Verifiche

Su QEMU (immagini del ramo): avvio normale fino al desktop su t23 e
desktop-2002; finestra DOS su desktop-2002 e desktop-1998; DPMI, audio-state
e audio delle VM sui tre profili; video ×3; DOS/4GW 20 cicli su t23 e
min128; gate di starvation; DOOM 5 cicli completi (1710 gametics ciascuno);
Wolf3D al signon e alle fasi 1–3. QEMU 11.1 stesso è caduto tre volte in
`tb_invalidate_phys_range_fast` durante i carichi DOS (deviazione
registrata): i casi vengono ripetuti per i pin. Aperti: DOOM con audio non
raggiunge il modo grafico; Wolf3D oltre la fase 3 (interrotto da QEMU).

## Decisioni

- Il design visivo nuovo (temi Platinum, Ciuki Blue, Classic 98; font Inter
  e Inconsolata rasterizzati dalle sorgenti OFL) si integra nella shell con
  un passaggio dedicato prima della campagna finale: nulla entra in `main`
  senza screenshot al proprietario.
- Il set di icone unico resta Tango 0.8.90 (pubblico dominio, completo).
- Gli accessori stanno sotto Programmi → Accessori; i programmi sconosciuti
  sotto Sviluppo.
