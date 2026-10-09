# Revisione dell'architettura: fondamenta ispirate a Windows 95

- **Data:** 2026-10-09
- **Tipo:** decisione
- **Versione/commit:** nessuno (nessun codice modificato)
- **Stato:** proposta, in attesa della decisione del proprietario

## Contesto

Il proprietario vuole rivedere l'intera architettura per avere un sistema
stabile anche su hardware reale, non solo su emulatore. Come riferimento ha
indicato l'architettura di Windows 95
([Adrian King, *Inside Windows 95*, 1994](https://vtda.org/books/Computing/OperatingSystems/Inside_Windows_95_Adrian_King_1994.pdf))
e ha proposto di ripartire dalla 0.8.5 ristrutturando le fondamenta.
Questa analisi si basa sul codice, sui documenti del repository e sul
diagramma dell'architettura di Windows 95; il libro va ancora riletto
capitolo per capitolo.

## Diagnosi: oggi CiukiOS somiglia a Windows 3.1 in modalità 386 avanzata

- **Proprietario dell'hardware:** `CIUKIDOS.SYS`, kernel in modalità reale
  (43 KB, ora 60 KB nel nuovo layout), che usa il BIOS a runtime per input,
  disco e video.
- **Sopra:** JemmEx (monitor V86 di terzi, 9 patch) e il modulo JLM
  CVSESSION, che contiene scheduler delle VM, GPU, periferiche virtuali e
  disco ATA; HDPMI modificato per DOS/4GW.
- **Desktop:** `SHELL.COM` a 16 bit in modalità reale; app `.APP` limitate a
  64 KB sotto 1 MB.
- **VM DOS:** create da VMFORK copiando lo stato del processo padre.

In Windows 95 la gerarchia è opposta: DOS serve solo all'avvio, poi VMM32 a
ring 0 possiede CPU, memoria, interrupt e dispositivi; DOS diventa un ospite
dentro le VM; BIOS e driver in modalità reale restano solo come ripiego
(Real Mode Mapper). In CiukiOS la parte protetta è un plugin della parte in
modalità reale, e nessun componente possiede davvero lo stato condiviso.

## Collegamento con il report del 9 ottobre

| Sintomo | Causa architetturale | Come lo risolve Windows 95 |
| --- | --- | --- |
| DA=0000:0000 al secondo DOOM | stato condiviso nella memoria bassa fra padre e VM forkata, senza proprietario | primo MB privato per VM, dati di istanza gestiti dal VMM |
| S3: fill ok, copia 2D fallita | GPU pilotata dentro un plugin di Jemm, senza modello di driver con ripiego | mini-driver display con ripiego VGA/VESA, accelerazione disattivabile |
| Audio: 101 attese oltre limite, buchi di 12 ms | il mixer dipende dal guest che cede il controllo; nessuno scheduler con priorità | VxD a ring 0 con eventi e priorità |
| Input e video via BIOS a runtime | ogni BIOS reale si comporta diversamente da SeaBIOS | driver a 32 bit, BIOS solo come ripiego |
| Margini esauriti (23 byte in HDPMI, kernel pieno, app a 64 KB, 16 patch su codice di terzi) | crescita per strati sopra una base a 16 bit | spazio di indirizzamento a 32 bit flat |

Una ventina di note di design e altrettanti record di validazione dedicati al
solo T23 indicano che si stanno
inseguendo sintomi, non difetti isolati.

## Architettura proposta

Struttura di Windows 95, ma con isolamento della memoria più rigido, alla NT.

| Livello Windows 95 | CiukiOS oggi | Fondamenta nuove |
| --- | --- | --- |
| VMM a ring 0 | Jemm + JLM CVSESSION | **Ciuki VMM** proprio: GDT/IDT/paginazione, pagine dalla mappa E820, scheduler preemptivo con priorità, timer, eventi |
| Configuration Manager / Arbitrator | rilevamento sparso, `ACTIVE.CFG` | registro risorse: ogni IRQ, porta, MMIO o DMA ha un solo proprietario; enumeratore PCI |
| IFS Manager + VFAT + IOS | FAT16 nel kernel reale via INT 13h | FAT16/32 a 32 bit, driver ATA nativo; INT 13h solo come ripiego |
| Mini-driver display | GPU nel JLM | VBE LFB generico sempre funzionante; S3/ATI come mini-driver opzionali, spenti finché non qualificati |
| System VM (Kernel/User/GDI) | SHELL.COM 16 bit + `.APP` 64 KB | desktop e app come processi 32-bit flat a ring 3 (OpenWatcom 32 bit) |
| DOS VM | VMFORK + Jemm + HDPMI | VM V86 con primo MB privato avviato da un'immagine DOS pulita; INT 21h file verso l'IFS a 32 bit |
| MS-DOS all'avvio | CIUKIDOS.SYS | resta: avvio, raccolta dati BIOS (E820, VBE, EDID, PCI) e personalità DOS dentro le VM |

**Da non copiare:** l'area condivisa scrivibile da tutti e il Win16Mutex,
cause principali dell'instabilità di Win9x.

**Da copiare:** la modalità provvisoria (solo VBE, niente accelerazione,
niente audio, solo PS/2), una schermata di crash con dump dei registri salvata
su disco, e un log su porta seriale se T23 ed E500 la espongono.

**Lezione dalla prima generazione:** il kernel deve essere a **32 bit**, non
a 64: solo in modalità protetta a 32 bit la V86 è nativa (vedi
[prima generazione UEFI/x86_64](2026-04-15-01-prima-generazione-uefi-x86-64.md)).

## Cosa si riusa

- Circa 31.000 righe in C e assembly a 32 bit in `src/vm/`, che già girano a
  ring 0: driver GPU, VGA/SB/OPL virtuali, disco ATA, orologio. Sono il seme
  del VMM: vanno promosse da plugin di Jemm a kernel.
- Le app C (desktop, Files, Paint, browser, rete) e LFN, da portare a 32 bit.
- CiukiDOS, come personalità DOS dentro le VM.

Si sostituiscono SHELL.COM come host grafico, lo stack di patch Jemm/HDPMI,
VMFORK e il collegamento CVSESSION–Jemm.

## Piano proposto

1. Congelare l'architettura attuale in un ramo `legacy-0.8` (solo correzioni
   critiche); coincide con il possibile ramo solo-CiukiDOS.
2. Niente "big bang": ogni fase deve avviarsi su QEMU, T23 ed E500 prima della
   successiva.
   - **F0:** VMM minimo: passaggio da CiukiDOS alla modalità protetta, pagine
     E820, IDT, PIT, schermata di crash, log.
   - **F1:** driver nativi di base: PS/2, VBE LFB, ATA PIO + FAT, modalità
     provvisoria.
   - **F2:** processi a 32 bit a ring 3 e scheduler; port del desktop e di
     una prima app.
   - **F3:** una VM DOS V86, poi più VM, poi DPMI fino a DOOM.
   - **F4:** audio AC97/SB nativo con virtualizzazione; accelerazione S3/ATI
     opzionale.
3. Accettare che per un periodo la 0.8.5 faccia meno della 0.8.3, purché ciò
   che fa funzioni anche su hardware reale.

## Decisioni aperte

- VMM proprio oppure Jemm come base. Raccomandazione: VMM proprio, perché Jemm
  ha un solo blocco di controllo VM statico e lo scheduler è già una nostra
  patch; riusare la sua emulazione delle istruzioni V86 dove la licenza lo
  consente.
- Host DPMI integrato oppure HDPMI dentro ogni VM: rimandabile a F3.

## Prossimo passo

Se approvata, scrivere `docs/design/foundations-0.8.5.md` con la ricerca sulle
fonti (capitoli del libro su VMM, IFS e Configuration Manager; Intel SDM;
specifica DPMI), la mappatura componente per componente e i criteri di
accettazione su hardware reale, prima di toccare il codice.
