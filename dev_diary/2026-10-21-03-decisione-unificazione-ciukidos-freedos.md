# Decisione del proprietario: unificare CiukiDOS e FreeDOS in un unico DOS definitivo e completo

- **Data:** 21 ottobre 2026
- **Tipo:** decisione (architettura DOS)
- **Versione/commit:** 0.9.2 pre-alpha / main `d5deb57a`
- **Stato:** in vigore; sostituisce il punto 1 della voce 2026-10-19-06 (CiukiDOS resta il DOS, FreeDOS solo riferimento); i punti 2 e 3 di quella voce restano validi

## Decisione

Il proprietario chiede di fondere CiukiDOS e FreeDOS per ottenere un DOS definitivo e completo, lavorando in parallelo alle altre attività del programma.

Decisioni del lead, da verificare nel contratto:

1. Il nome e l'identità visibile all'utente restano CiukiDOS (banner di avvio, `VER`, prompt, shell). FreeDOS entra come base del kernel: fork del kernel FreeDOS 1.3 (GPL-2.0, costruito dai sorgenti con pin e licenza conservati, sorgenti offerti con l'immagine) in cui si portano le integrazioni di CiukiDOS con il VMM (binding DFVM versionato con ROM gate, FCB e NLS, canale di chiamate su file verso il VMM secondo il modello IFS Manager di Windows 9x, D2 del contratto delle risorse) e le funzioni proprie di CiukiDOS che FreeDOS non ha.
2. L'API file DOS resta servita dal VMM (voce 2026-10-19-06, punti 2 e 3); nel kernel unificato il punto di aggancio preferito è l'interfaccia del redirector di rete (INT 2Fh, AH=11h) che FreeDOS già implementa, da confermare nella ricerca.
3. L'attuale kernel CiukiDOS resta selezionabile come ospite finché il kernel unificato non supera al 100 % la batteria di conformità f3-48 e non avvia tutti i titoli della libreria DOS; dopo, il kernel attuale è ritirato con una voce di diario.
4. Programma: direttiva `f3-54` (contratto, inventario di CiukiDOS e del kernel FreeDOS, scelta del toolchain, licenze, piano delle direttive di implementazione; nessun codice di produzione), revisione incrociata, poi le direttive di implementazione scritte dal contratto, eseguite in parallelo alle attività F5, F6 e F7.

## Riferimenti

- Voce 2026-10-19-06; `docs/directives/f3-48`, `f3-49`, `f3-50`, `f3-51`; `docs/design/dos-dpmi-contract.md`; `scripts/dos_conformance.py` (riferimento FreeDOS 1.3 in `third_party/freedos/1.3`).
