# Integrazioni 8 e 9 su main, scadenza di avvio della sessione, versione della Beta

- **Data:** 2026-10-29 09:35
- **Tipo:** integrazione, decisione

## Fatti

1. **Integrazione 8** (ramo `wt/integ8`). Contiene:
   - l'integrazione 7;
   - LAN dal vivo e IPX (lan2, f6-28);
   - Radeon (f7-24);
   - sec-k08b;
   - runtime di Half-Life (fx-hlrun);
   - correzioni Win32 (fx-w32green);
   - giochi DOS (fx-games20b, fx-games20c: 20 giochi fissati, installer e configurazione del suono);
   - sec-firstboot round 1.

   Le suite host hanno trovato:
   - il link mancante della configurazione IPX nel test dei servizi di sistema;
   - la riga di census di `CIUKI_SYSTEM_SETTING_IPX_FRAME`;
   - il budget di acc-t23 superato dai nuovi casi;
   - `DLL_PROCESS_ATTACH` di `dplayx.dll` fallito nell'harness H1;
   - fixture desktop senza il controllo dei firmware Radeon e senza `sectest`.

   La direttiva integ8-glue (Codex, 2fd7123e7) li ha corretti. Il DllMain di `dplayx` costruisce solo i dati condivisi, come in Wine, e l'inizializzazione IPX/TCP passa alla prima chiamata del service provider.
2. **Decisione K8-8: scadenza di avvio della sessione da 5 s a 30 s** (`CIUKI_SECURITY_LAUNCH_TIMEOUT_MS`, `docs/directives/sec-lead-decisions.md`).
   - Sul profilo `qemu-t23` (TCG Pentium III) i primi avvii dell'integrazione 8 fallivano: il desktop, che segnala di essere pronto dopo il primo fotogramma composto, superava i 5 s e il kernel chiudeva l'avvio con `-ETIMEDOUT`.
   - Riferimento: il Service Control Manager di Windows usa 30.000 ms come attesa di avvio di un servizio (`ServicesPipeTimeout`, evento 7011), mentre Winlogon non impone una scadenza alla shell.
   - Il test host verifica il fallimento con `-ETIMEDOUT` alla nuova scadenza e non prima.
3. **Integrazione 9 su main** (merge 2889623e2). Contiene:
   - integrazione 8 e integ8-glue;
   - f7-unify;
   - sec-firstboot round 2;
   - fx-uaccounts round 2;
   - utility Sierra di Half-Life (fx-hlutils);
   - fx-hlis5 round 2.

   La direttiva integ9-glue (Codex, b281b4547) aggiunge:
   - le righe H1 di attach di `wininet`;
   - la riga di census di `COM_CLEAR_ERRORS`;
   - le cartelle shell di cache, cookie e cronologia di WinINet nel resolver e negli hive predefiniti.

   Nella risoluzione del conflitto su `config/wine-upper.json`, un `checkout --ours` dell'intero file aveva perso 271 righe di fx-hlutils. `lost-lines.py` le ha rilevate e il file è stato ricostruito con `git merge-file`, risolvendo solo il blocco in conflitto.
4. **Verifiche dell'integrazione 9:**
   - build canonica `7d165424…`;
   - suite host security verde e riferimento API allineato;
   - smoke verde, e avvii `qemu-t23` e `qemu-desktop-2002` fino alla sessione 1.
   - Nella suite Win32 il test della seam DirectDraw è fallito una volta, al passo clipboard (errore 120), e passa da solo.

   `merge-verify.sh` su main (build del kernel, app, ABI, desktop, suite kernel) è verde. main è su origin a 537e22e69; la pubblicazione resta sospesa da `config/release-policy.json`.
5. **Test instabile della clipboard** (fx-flaky-clip, Codex). Il test rifiutava un `CF_METAFILEPICT` ricostruito allo stesso indirizzo del blocco liberato dal proprietario. Il `GlobalAlloc` host non ha bit di generazione, quindi il riuso dell'indirizzo è legittimo. Il test ora forza quel riuso e confronta i byte esatti del metafile.
6. **Decisione del lead: la 1.0 Beta è la versione `1.0.2`, stage `beta`.**
   - La sezione 1.0.1 alpha del CHANGELOG non è mai stata pubblicata e resta com'è.
   - `release_stable.py --channel beta` crea il tag `v1.0.2-beta.<n>`.
   - La preparazione (versione, CHANGELOG, note di rilascio, scelta delle note per canale) è la direttiva fx-beta-prep.
