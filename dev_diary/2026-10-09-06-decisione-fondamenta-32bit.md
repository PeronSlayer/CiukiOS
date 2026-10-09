# Decisione: nuove fondamenta a 32 bit in stile Windows 95/98

- **Data:** 2026-10-09
- **Tipo:** decisione
- **Versione/commit:** nessuno (decisione; transizione avviata)
- **Stato:** decisa da Claude e Codex su mandato del proprietario; prossimo
  passo: contratti di design prima del codice di F0

## Contesto

Requisito del proprietario: "io voglio avere un'architettura stabile e
funzionale come windows 95/98 e passare definitivamente al 32bit (e al FAT32,
NTFS ecc)". Il proprietario ha chiesto a Claude e Codex di discuterne e
decidere insieme.

La discussione si è svolta in tre turni sullo stesso thread Codex
(`gpt-6-astra`, effort `xhigh`, sola lettura):
1. bozza di decisione di Claude (D1–D11);
2. risposta di Codex: d'accordo con modifiche su tutti i punti, disaccordo
   su D10;
3. Claude ha accettato le modifiche; Codex ha confermato **"FINAL: AGREE"**.

Con questo requisito Codex supera la sua raccomandazione precedente
([2026-10-09-05](2026-10-09-05-revisione-codex-architettura.md)). La decisione
non stabilisce però le cause dei guasti osservati sul T23.

## Decisione

**D1 – Kernel.**
- Nuovo kernel in modalità protetta a 32 bit, a ring 0. Niente long mode,
  niente PAE e un solo processore all'inizio.
- Dopo l'avvio possiede CPU, paginazione, IDT/PIC/PIT, memoria fisica (mappa
  E820) e dispositivi.
- Jemm viene ritirato nella nuova linea. Il suo codice si riusa solo dove la
  licenza del singolo file lo consente (inventario delle licenze file per
  file).

**D2 – Avvio.**
- Si mantiene il percorso di avvio BIOS, con un loader rifatto: struttura di
  avvio versionata, kernel caricato sopra 1 MiB, riserve esplicite per
  loader, kernel, firmware e framebuffer.
- Le strutture restituite dal firmware vengono copiate prima di riusarne la
  memoria; l'assenza di EDID è un caso normale.
- `CIUKIDOS.SYS` non possiede più il sistema.

**D3 – Toolchain.**
- clang + ld.lld + NASM, già installati; verificato un ELF32 freestanding.
- Versioni bloccate, linker script con indirizzi fisici e virtuali,
  convenzione di chiamata e allineamento dello stack fissati, nessun x87 nel
  kernel, helper di runtime verificati.
- OpenWatcom/ia16 restano solo per i componenti DOS.

**D4 – Processi.**
- Spazio di indirizzamento privato per ogni processo; nessuna area condivisa
  scrivibile in stile Win9x.
- Formato ELF32 statico; PE32 solo più avanti, come personalità di
  compatibilità Win32.
- Mappature del kernel solo supervisor, controllo dei buffer utente, pagine di
  guardia, pulizia dei processi, stato FPU, oggetti di memoria condivisa
  espliciti.

**D5 – Dispositivi.**
- Registro delle risorse: PCI, IRQ condivisi, porte, MMIO, DMA.
- Driver nativi:
  - PS/2, PIT/RTC;
  - ATA (PIO, poi DMA) e ATAPI;
  - framebuffer VBE (modo impostato all'avvio);
  - AC97 per il T23 ed ESS Maestro-2E (125D:1978) per l'E500.
- Il BIOS è solo un ripiego, chiamato con i dispositivi fermi.
- L'accelerazione S3/Mach64 è opzionale e spenta finché non è qualificata.
- Resta la regola ATA: nessun ripiego sul BIOS dopo un comando già inviato.

**D6 – Filesystem.**
- VFS con offset a 64 bit, una sola cache, lock e semantica di flush/errore.
- FAT32 con nomi lunghi come filesystem nativo da F1: prima la lettura
  qualificata, poi la scrittura. Il layout di avvio va rifatto, perché quello
  attuale si sovrappone a FSInfo e al settore di boot di backup.
- FAT12/16 per compatibilità; ISO9660 per il CD.
- NTFS in sola lettura più avanti; la scrittura richiede una decisione
  separata.
- Licenza GPLv2: si può riusare codice GPLv2 o "GPLv2 o successive", non
  codice solo GPLv3.

**D7 – DOS.**
- VM V86 con primo MiB privato, avviate da uno stato pulito e non dal fork di
  un padre.
- Il kernel fornisce DPMI 0.9 completo (callback, cambi di modo annidati, IF
  virtuale, riflessione delle eccezioni, pulizia), come faceva Windows 9x.
  Niente VCPI né HDPMI; gli extender non supportati sono esplicitamente fuori
  garanzia.
- CIUKIDOS fa da personalità DOS, inizialmente invariato, su un disco
  virtuale FAT16 privato e con firmware e dispositivi virtuali.
- Il ponte verso la VFS va dentro il DOS (handle, PSP/JFT/SFT, DTA, FCB),
  aggirando le cache FAT del guest per i volumi condivisi; l'accesso a
  settori grezzi sui volumi condivisi è rifiutato.

**D8 – Desktop.** Il desktop e il compositor diventano un processo a ring 3.
Si porta prima un'app e si dimostra che un suo crash non uccide né il
desktop né gli altri processi. Gli asset di Ciuki approvati restano.

**D9 – Robustezza.**
- Modalità provvisoria e ripiego testo/seriale quando non c'è un LFB usabile.
- Il gestore di panic funziona senza allocazioni, lock o disco.
- Il log su disco viene scritto solo dopo l'inizializzazione dello storage.

**D10 – Transizione.**
1. Il branch `legacy-0.8` conserva `868cac9`; b849 è la release 0.8 di
   riferimento. Eventuali fix critici possono portare a una nuova release
   legacy.
2. Le decisioni di architettura sono committate a parte.
3. Una politica di pubblicazione versionata
   (`config/release-policy.json`) sospende le prerelease di `main`; l'hook
   resta attivo e continua a controllare che il working tree sia pulito.
4. Il percorso build/avvio viene sostituito **in modo atomico** da uno
   scaffold F0 funzionante. Modelli riusabili, app, test, asset, licenze e
   dipendenze del build DOS restano finché non vengono migrati o ritirati uno
   per uno.
5. La pubblicazione riprende dopo F1, con un pacchetto Windows riqualificato
   sulla nuova immagine.

**D11 – Prima del codice di F0** vanno concordati i contratti in
`docs/design/`:

| Documento | Autore | Revisore |
| --- | --- | --- |
| `foundations-transition.md`: perimetro, riuso e licenze, branch, release | Claude | Codex |
| `boot-memory.md`: loader e layout FAT32, E820, riserve, budget a 128 MiB | Claude | Codex |
| `execution-abi.md`: toolchain, ELF32, syscall, scheduler, lock, FPU, pulizia | Claude | Codex |
| `vfs-storage-contract.md`: cache, offset, handle, lock, flush ed errori | Claude | Codex |
| `dos-dpmi-contract.md`: DPMI, callback, A20/HMA/XMS/EMS/UMB, ponte VFS | Codex | Claude |
| `device-firmware-ownership.md`: PIC/EOI, IRQ condivisi, DMA, BIOS, ACPI/APM | Codex | Claude |
| `f0-acceptance.md`: sonde, selezione su hardware, integrazione con il runner | Codex | Claude |

## Criteri di accettazione di F0 (concordati)

- La stessa immagine HDD canonica si avvia ripetutamente su SeaBIOS, T23 ed
  E500; QEMU funziona anche con 128 MiB.
- La struttura di avvio versionata convalida i limiti del kernel, le voci
  E820 e le riserve, e registra i totali usabili e riservati.
- L'esaurimento dell'allocatore fallisce in modo pulito; cicli ripetuti di
  allocazione e rilascio ripristinano la contabilità senza toccare la memoria
  riservata.
- Modalità protetta, paginazione, GDT/IDT/TSS e stack del kernel con pagine
  di guardia funzionano; sono verificati le mappature supervisor e CR0.WP.
- Due sonde a ring 3 usano gli stessi indirizzi virtuali su pagine diverse,
  e i loro valori sentinella sopravvivono ai cambi di contesto.
- Due sonde che non cedono mai il controllo completano 10.000 cambi guidati
  dal timer senza starvation né corruzione di registri o stack.
- Accesso alla memoria del kernel, istruzioni privilegiate e I/O non
  autorizzato causano un fault locale; l'altra sonda continua.
- Buffer non validi passati alle syscall restituiscono errori; 100 cicli di
  creazione/fault/uscita ripristinano pagine e risorse.
- Un fault del kernel riporta vettore, codice d'errore, EIP e CR2 su seriale
  o schermo e si ferma senza accedere al disco; l'assenza della porta seriale
  non blocca l'avvio.
- L'uso della FPU segue la politica dichiarata.
- Il runner applica limiti di risorse, serializzazione, timeout, prove
  SHA-256 e pulizia degli overlay. Su QEMU le sonde si scelgono con fw_cfg;
  sull'hardware le stesse sonde si scelgono dal menu di avvio o via seriale
  (niente fw_cfg né qcow2 sui PC reali).

## Prime tre priorità (Codex)

1. Un unico modello di proprietà per memoria, interrupt, scheduling, DPMI e
   ciclo di vita dei dispositivi.
2. Un contratto di avvio e storage verificato, incluso il layout FAT32 e il
   budget misurato a 128 MiB.
3. Test di isolamento e di guasto presto sull'hardware reale, specialmente
   l'input dell'E500 e l'avvio/chiusura ripetuti delle VM DOS.
