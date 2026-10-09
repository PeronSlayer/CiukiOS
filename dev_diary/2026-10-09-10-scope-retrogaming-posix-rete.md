# Scope del progetto: OS moderno per il retrogaming, API POSIX, rete

- **Data:** 2026-10-09
- **Tipo:** decisione (scope)
- **Versione/commit:** commit di questa voce
- **Stato:** decisa dal proprietario; da specificare nei contratti prima di
  F2 (POSIX) e F6 (rete)

## Contesto

Dopo F0 su QEMU (smoke superato, core 55/56; safe-mode automatico e
qualificazione hardware ancora aperti) il proprietario ha fissato l'obiettivo
del progetto: non battere Windows 98 in compatibilità, ma **un sistema
moderno per il retrogaming** che resti un progetto attivo, con una piccola
comunità alle fiere del retro computing e LAN party con i giochi dell'epoca;
con l'ambizione di arrivare ai giochi e alle applicazioni Win32 fino a
Windows ME (Half-Life, Quake 3), alla rete completa e a una base hardware
solida come quella di Windows 9x.

## Confronto con ReactOS

ReactOS reimplementa Windows NT in modo binario-compatibile (kernel, Win32,
driver): ventotto anni, ancora alpha. CiukiOS non lo fa: kernel proprio con
architettura Windows 95, compatibilità binaria solo verso il DOS, API
native proprie. I progetti paragonabili sono Haiku, KolibriOS e SerenityOS.

## Decisioni

1. **Scope dichiarato** (README e Roadmap, con parole semplici): OS open
   per giocare ai giochi degli anni '90 sui computer degli anni '90;
   giochi DOS nativi; motori con sorgenti pubblici compilati per CiukiOS (Quake,
   Half-Life via Xash3D, Doom, Descent, Duke3D); LAN party;
   più avanti i giochi Windows 95/98 originali tramite Wine e il 3D su chip
   scelti. Non è un clone di Windows.
2. **API nativa POSIX-compatibile** in F2 al posto di un'API inventata:
   syscall del kernel che offrono i mattoni POSIX (processi, thread, mmap,
   segnali, file, socket, tempo, una primitiva di attesa simile a futex come
   estensione), una libc portata (newlib o musl), sopra SDL, lwIP, Mesa e
   infine Wine. È la presa standard che riduce il lavoro di port; Wine
   richiederà comunque un backend CiukiOS e servizi aggiuntivi.
   L'architettura decisa (kernel che possiede la macchina, VM DOS, driver
   nativi) non cambia.
3. **Rete** come fase propria (F6): lwIP, DHCP, driver di rete tramite uno
   strato di compatibilità (modello Haiku/FreeBSD), scheda virtuale con IPX
   e packet driver nelle VM DOS, socket nativi; winsock via Wine in F8.
4. **Scala in nove gradini** (F0 più F1–F8), ognuno con capacità
   dimostrabili e testate: fondamenta, driver e disco, programmi nativi,
   DOS, audio e media, giochi nativi, rete, 3D, giochi Windows.
5. **"La forza di Windows 9x sull'hardware"** non si ricrea: si prende in
   prestito dai driver open esistenti (DRI/DRM di Mesa per il 3D, driver
   FreeBSD/Linux per le schede di rete) e si limita a una matrice di
   hardware scelta e qualificata.

## Vincoli noti

Parti difficili: F3 (monitor V86 e DPMI), F7
(driver 3D rimossi da Mesa: savage/mach64/r128/tdfx in Mesa 8.0 del 2012,
nouveau_vieux in Mesa 22.0 del 2022; le prestazioni 3D del SuperSavage
IX/C del T23 e della Rage Mobility dell'E500 non sono misurate), F8
(dimensione di Wine, che richiede un backend CiukiOS proprio: POSIX riduce
il lavoro ma non garantisce Wine). Licenze: Quake 1/2 e ioquake3 sono
GPL-2.0-or-later; Xash3D-FWGS è GPL-3.0-or-later e si distribuisce come
programma separato; DevilutionX ha una "Sustainable Use License" con
restrizioni commerciali e non è open source in senso stretto: candidato a
parte.

## Ruoli (10 ottobre)

Claude dirige (direttive scritte, criteri di accettazione, revisione,
integrazione, prove su QEMU e hardware, commit); Codex scrive il codice nel
suo worktree, con modello ed effort scelti da Claude in base alla
complessità. Il kernel F0 è stato scritto da Claude, loader e runner da
Codex; da qui in poi il codice è di Codex. Dettagli in
[2026-10-10-01](2026-10-10-01-direttive-f1-codex.md).

## Da fare prima del codice

- `execution-abi.md`: sezione F2 con il sottoinsieme POSIX (tabella delle
  syscall, segnali, thread 1:1, mapping dei percorsi, libc).
- Nuovo contratto di rete per F6 e scheda virtuale IPX/packet driver nel
  contratto DOS.
- Revisione di Codex sui testi di scope e sui contratti.
