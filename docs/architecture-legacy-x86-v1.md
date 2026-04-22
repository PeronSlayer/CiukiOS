# Architettura CiukiOS Legacy x86 v1

## 1. Obiettivo architetturale
Architettura nativa x86 legacy (BIOS), senza dipendenza UEFI e senza layer di emulazione CPU per l'esecuzione DOS/pre-NT.

## 2. Principi non negoziabili
1. Boot da BIOS legacy reale.
2. Esecuzione software DOS nativa su CPU reale.
3. Compatibilita incrementale verificabile con milestone concrete.
4. Due profili prodotto: `floppy` e `full`.

## 3. Strati del sistema
1. Stage-B0 (Boot Sector): loader minimo 16-bit (512B), salto a stage successivo.
2. Stage-B1 (Loader esteso): init memoria base, filesystem boot media, caricamento kernel.
3. Stage-K (Kernel core): scheduler semplice/event loop, interrupt routing, memory manager.
4. Stage-D (DOS native runtime): loader COM/EXE, PSP/MCB, INT 21h/10h/13h/16h/1Ah/33h.
5. Stage-G (Grafica/Desktop): VGA/VBE, VDI/AES compat nativo, OpenGEM path.
6. Stage-A (Applicazioni): tool DOS, DOOM, target Windows pre-NT.

## 4. Modello di esecuzione
1. Bootstrap iniziale in real mode.
2. Passaggio controllato in protected mode dove utile.
3. Uso v86/protected constructs solo hardware-native (niente interpreter CPU).
4. Servizi DOS esposti tramite interrupt compatibili.

## 5. Profili build
## 5.1 Profilo `floppy`
1. Target: 1.44MB (`FAT12`) avviabile BIOS.
2. Contenuto: kernel minimo + shell + diagnostica + subset DOS API.
3. Uso: bring-up hardware, debug base, recovery.

## 5.2 Profilo `full`
1. Target: immagine estesa (`FAT16/32`) per runtime completo.
2. Contenuto: stack grafico completo, OpenGEM, tool DOS avanzati, target app complessi.
3. Uso: ambiente operativo completo.

## 6. Compatibilita target
1. DOS apps: priorita alta.
2. OpenGEM desktop: milestone grafica primaria.
3. DOOM: milestone performance/compat.
4. Windows pre-NT (fino a 98): percorso progressivo, non monolitico.

## 7. Requisiti di qualita
1. Logging seriale deterministico per boot e interrupt critici.
2. Test automatici per ogni milestone.
3. Verifica su hardware reale in aggiunta ai test su emulatori.

## 8. Cosa viene escluso
1. Dipendenza UEFI nel nuovo core runtime.
2. Soluzioni finali basate su emulazione CPU software.
3. Scope Windows NT e successivi.
