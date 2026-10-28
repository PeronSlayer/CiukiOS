# Decisione: utenti reali con ruoli di amministratore applicati dal kernel (modello Windows XP)

- **Data:** 2026-10-26
- **Tipo:** decisione
- **Versione/commit:** contratti e primi passi nei rami `wt/sec-model`, `wt/sec-kernel`, `wt/sec-k01`, `wt/sec-user-01`, `wt/sec-int01`, `wt/sec-k02` (non ancora in main)
- **Stato:** in corso (integrazione dopo l'immagine T23 successiva)

## Contesto

Gli utenti di G32 avevano profili e password, ma il kernel non distingueva
amministratori e utenti standard: ogni processo poteva scrivere ovunque.
Il proprietario ha chiesto un sistema di utenti e autenticazione reale.

## Decisioni del proprietario

- Ruoli applicati dal kernel, sul modello di Windows XP.
- **CiukiUser** è l'amministratore predefinito; le immagini pronte
  accedono automaticamente come CiukiUser, senza password.
- **CiukiAdministrator** è il super amministratore: disattivato e nascosto,
  usabile in modalità provvisoria con la sua password.
- **Guest** esiste ma è disattivato; senza Guest attivo non c'è accesso
  senza utente e password validi.
- **Setup** obbliga a creare un utente personalizzato, che diventa il primo
  amministratore; le installazioni da Setup non creano CiukiUser.

## Decisioni tecniche (revisione incrociata dei due contratti)

- CiukiAdministrator (RID 500) può autenticarsi in modalità provvisoria
  anche se disattivato; sulle immagini pronte resta senza password
  (non autenticabile) finché un amministratore non ne imposta una.
- Password di 8-63 byte stampabili (minimo NIST SP 800-63B); unica
  eccezione vuota: CiukiUser delle immagini pronte.
- SID di macchina generato al primo avvio con entropia, prima di
  pubblicare gli account.
- Radice del volume come Windows XP: gli utenti standard creano cartelle e
  file di primo livello (installer e giochi in `C:\GAME`) e ne diventano
  proprietari creatori; non modificano voci altrui né gli alberi di sistema.
- Profili privati in `/Windows/Profiles/u%08x`, metadati SYSTEM in
  `/System/Security`; migrazione esplicita dei profili G32 a uid ≥ 1000.
- Una famiglia di chiamate `SYS_SECURITY` (identità propria e del peer;
  creazione di sessione, spawn-as e fine sessione solo per SYSTEM) con una
  facciata Win32 di token e SID in sola lettura.
- Un `w32srv` per sessione utente più un broker SYSTEM del registro di
  macchina; il desktop non gira mai come SYSTEM.
- Letture e scritture grezze del disco di sistema rifiutate a ogni utente
  finché manca una classificazione esatta dei settori; ScanDisk,
  Deframmentazione e Setup passano da servizi SYSTEM dopo il controllo da
  amministratore.
- Spegnimento e riavvio consentiti a ogni sessione interattiva e dalla
  schermata di accesso; nessun blocco permanente dell'account, attesa
  limitata di 1 s tra i tentativi.
