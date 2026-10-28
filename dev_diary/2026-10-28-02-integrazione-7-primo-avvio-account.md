# Integrazione 7 su main e primo avvio degli account sul profilo lento

- **Data:** 2026-10-28 19:02
- **Tipo:** integrazione

## Fatti

1. **Integrazione 7 su main** (merge 776500066). Contiene sec-runtime1, sec-k08, sec-user-06, fx-games20 e f7-26 (NVIDIA solo software). Verifiche sul ramo di integrazione:
   - suite host kernel e security verdi;
   - suite desktop verde dopo la rigenerazione dell'inventario delle icone (riga `emblem-readonly` della shell);
   - suite Win32 con le sole due failure note di main: SMF formato 0 e census committabile, affidate a fx-w32green;
   - `lost-lines.py` senza perdite.
2. **Build canonica di main riuscita.** Il boot di default arriva al desktop. Sul profilo `qemu-t23` (TCG Pentium III) il primo avvio restava invece su "Logon is not available".
   - Il generatore di entropia del kernel diventa pronto a circa 13,4 s, dopo l'apertura di ACCOUNT. La promozione del seed riceveva EAGAIN da `getentropy` e non veniva ripetuta.
3. **Correzioni del lead:**
   - f09406018, ripetizione della promozione sul servizio aperto: insufficiente, il secondo tentativo restituiva EIO;
   - 4cba53067, su EAGAIN ACCOUNT chiude il servizio e lo riapre dopo 100 ms, con test host in `adapter_test.c` e nel caso H15 con l'OWNER del kernel.

   Con 4cba53067 un primo avvio `qemu-t23` ha promosso il seed ed è arrivato alla sessione 1. Il successivo, sulla stessa immagine (4011557a), si è fermato su `open refused ... EIO: -5` dopo l'apertura dello store OWNER: c'è una seconda fonte di EAGAIN dipendente dai tempi, dopo il RESERVE. Una possibile fonte è `owner_vfs_tree_digest`, che risponde EAGAIN se il volume cambia durante la scansione.

   Direttiva sec-firstboot (Codex): la promozione deve riuscire a ogni primo avvio, qualunque sia l'ordine tra entropia, store OWNER e scritture concorrenti. I seriali sono in `build/lead-scratch/sec-firstboot/`.
4. **Runner** (4fadbd448): sulle immagini con LOGON lo smoke normal-boot attende la riga `[logon] session N running` prima di catturare lo schermo. Prima lo catturava 8 s dopo il pid del desktop, mentre LOGON mostrava ancora "Loading your personal settings".
5. **Decisione K8-7.** Il timer del disco della sessione desktop attiva lo programma l'utente della sessione, come nella USER_POWER_POLICY di Windows 2000/XP (`SpindownTimeoutAc/Dc`). sec-k08b lo implementa ed è in integrazione 8.
6. **Integrazione 8.** Raccoglie integrazione 7, LAN (f6-25, f6-26, f6-27), Radeon (f7-24), sec-k08b, fx-hlrun e main. Correzioni del lead su questo ramo:
   - riga `CIUKI_SYSTEM_SETTING_IPX_FRAME = 6` nell'oracolo ABI e nel riferimento API;
   - stub IPX nella fixture di latenza del display.
7. **Half-Life nell'harness H1** (fx-hlrun):
   - installazione dal CD originale;
   - chiave CD inserita;
   - menu principale completo;
   - mappa t0a0 nel renderer software (DirectDraw 640x480x16) e in OpenGL (640x480x32).

   Le correzioni Win32 sono verificate su Wine 10.0 e sulla documentazione Microsoft:
   - repaint dei controlli static;
   - `ShowWindow` senza effetti quando non cambia nulla;
   - `ShowCursor`/`ClipCursor(NULL)` senza richieste quando non cambia nulla;
   - fine frame RLE nel codec MSRLE.
8. **main su origin** a d253e8224. La pubblicazione delle prerelease resta sospesa da `config/release-policy.json`.
