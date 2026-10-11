# f3-03 completata dal lead: servizi di memoria XMS/EMS/UMB e primo avvio delle VM DOS su QEMU

- **Data:** 2026-10-11
- **Tipo:** modifica (implementazione e correzioni) e analisi di regressione
- **Versione/commit:** branch `wt/f3-vm-memory` fuso su `main` dopo `3a19fd4`; immagine del worktree `f47a4d78…`
- **Stato:** in corso (campagna sull'immagine integrata da eseguire)

## Contesto

Codex ha esaurito il credito l'11 ottobre; per direttiva del proprietario il
lead ha completato f3-03 (memoria delle VM DOS: A20/HMA, XMS 3.0, UMB,
EMS 4.0, aritmetica di ammissione) nel worktree `wt/f3-vm-memory`. La riga
f3-00 era stata fusa con "QEMU evidence pending": nessuna VM DOS era mai
arrivata al prompt su un'immagine integrata. Sull'immagine main 38a7b4e7
`vm-boot` va in panic alla creazione della VM (`mutex: recursive acquisition
or lock order`: il kmutex `disk_lock` della VM contro il mutex del canale ATA
introdotto da f4-02).

## Cosa cambia

Cinque difetti risolti, in ordine di scoperta:

1. Ordine dei lock: il percorso disco della VM usa un gate `kwait` per VM
   (`disk_busy`/`disk_wait`, test-and-set in sezione critica) e non tiene
   più un kmutex attraverso la chiamata allo storage; `kmutex_lock` registra
   `[mutex] held= new= task=` prima del panic.
2. Scrittura in ROM allo splash: CIUKIDOS esegue INT 10h AX=4F02 per lo
   splash VESA; il BIOS sintetico risponde 014F; il gancio INT 10h del kernel
   ripiega sull'emulazione VBE locale e alloca il backing store senza PSP
   attivo; `int21_mem_arena_start` restituiva 0 e il primo MCB finiva a
   FFFF:0000. Il firmware reale ignora le scritture in ROM; le pagine ROM
   della VM sono di sola lettura e ogni VM si fermava con
   `vector=14 address=000ffff0 error=7`. L'arena parte da `DOS_HEAP_USER_SEG`
   senza PSP. Nello stesso installer i vettori 60h–67h non sovrascrivono più
   INT 67h quando il segmento del gestore porta `EMMXXXX0` (LIM EMS 4.0 §3.3).
3. Directory di lavoro: CIUKIDOS crea `\SYSTEM` e `\APPS` e la shell parte in
   `C:\APPS`; COMMAND_COMPAT risolve i nomi solo nella directory corrente, per
   cui `DOSREADY.COM` di AUTOEXEC non veniva mai trovato. L'immagine DOS e le
   sonde usano percorsi assoluti.
4. Larghezza dell'operando OUT: `v86_emulate` passa l'intero EAX al gestore
   delle porte e `dosvm_io` confrontava quel valore con i byte di comando
   (D0h, DDh, DFh, FEh, lo zero della porta di controllo); con AH diverso da
   zero la risposta D0h del controller tastiera non si latchava (VMMEMORY tag
   A7) e l'uscita della shell avrebbe fermato la VM con un fault. Il gestore
   usa solo il byte scritto; l'harness host ripete le scritture con AH sporco.
5. Selettore: la grammatica F3 rifiutava ogni suffisso, ma il profilo
   qemu-e500 aggiunge `platform=e500` a ogni selettore, quindi nessun caso F3
   poteva girare su e500. Loader (`menu.inc`), parser del kernel
   (`selector.c`) e modello del runner (`loader_model.py`) accettano ora
   l'override di piattaforma per F3 come per F1/F2; `safe=1` resta vietato.

Diagnostica aggiunta ai record: `group=vm_stop` porta `address`, `error` e
`code` (otto byte a CS:IP); ogni arresto diverso da EXIT emette
`group=vm_fault` (registri) e `group=vm_stack part=0..2`; una VM che non
raggiunge il prompt emette `group=boot_state` e `group=screen` (schermo
testo). CR2 viene letto prima di riabilitare le interruzioni. VMMEMORY.COM
riporta `VMMEMORY FAIL nn` con la funzione XMS/EMS o il controllo A20 fallito.

Sonda `f3:vm-memory`: avvia una VM da 32 MiB, lancia `\VMMEMORY.COM`,
latcha il verdetto dalla riga E9, crea una seconda VM registrando
l'aritmetica di ammissione; casi `vm-memory-qemu-t23/e500/min128` con
l'ammissione della seconda VM fissata a 1.

Test host: `test_memory_services` copre XMS 00h–12h/88h/8Fh, il validatore
di move, UMB 10h–12h, EMS 40h–51h con alias di pagina per finestra, la
distruzione con alias ancora mappati e il fallimento del backing in
creazione; harness `dosvm_test` PASS (166487 controlli).

## Verifiche

Immagine del worktree f47a4d78: `vm-memory-qemu-t23`, `-e500` e `-min128`
PASS (seconda VM da 32 MiB ammessa su tutti i profili: su min128 116 MiB
liberi prima della prima VM e 91 MiB con 32 MiB riservati e 25 MiB residenti
prima della seconda; `xms_calls=007fffff ems_calls=3fffffff`; entrambe le VM
escono con reason=0). `vm-boot-qemu-t23` PASS sulla stessa immagine: 100
cicli puliti e 100 con fault (50 istruzione privilegiata, 50 porta vietata),
isolamento IVT/disco del testimone, heartbeat nativo con 98543 dispatch,
baseline ripristinate (127760 pagine, 2 handle, 3 device). Harness C del
kernel e test statici del loader PASS nel worktree.

Da eseguire sull'immagine integrata: `f3-vm` completo (sei casi),
`f0-smoke`, `f2-all`; poi l'immagine hardware n. 6.

## Decisioni

- Le pagine ROM della VM restano di sola lettura (contratto f3-00): la
  personalità DOS non deve scrivere in ROM; il difetto si corregge nel kernel
  DOS, non allentando la protezione.
- I nomi dei payload DOS sono assoluti nell'immagine e nelle sonde.
- Dopo ogni fusione che tocca `src/kernel/vm`, `ata.c` o `sync.c` si esegue
  `vm-boot-qemu-t23` prima di ogni altra qualifica.
- La riga f3-01 (`wt/f3-vm-devices`) riceve queste correzioni fondendo
  `main` nel suo worktree prima della sua qualifica.
