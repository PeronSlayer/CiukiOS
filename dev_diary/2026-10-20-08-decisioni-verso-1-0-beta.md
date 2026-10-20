# Decisioni del proprietario verso la 1.0 Beta: rete in parallelo, QEMU per rete e 3D, OpenGL, Win16 dopo la beta

- **Data:** 2026-10-20
- **Tipo:** decisione
- **Versione/commit:** 0.9.2 pre-alpha / main `928b893b`
- **Stato:** decisa

## Contesto

Il proprietario ha fissato gli obiettivi della 1.0 Beta: rete con accesso a internet tramite un browser open source, partite in LAN con giochi dell'epoca, giochi DOS sempre funzionanti con audio e LAN, un gestore dei programmi installati che permetta di disinstallarli (anche i giochi freeware inclusi). L'elenco dei titoli e dei siti di prova è privato e resta fuori dal repository (`legacy/local/`).

## Decisioni

1. **Browser:** NetSurf, port nativo con il frontend framebuffer, sull'API POSIX di F2 e sullo stack di rete di F6.
2. **Il 3D fa parte della 1.0 Beta** (F7). Per la beta il 3D è OpenGL; Direct3D tramite wined3d viene dopo la beta.
3. **F6 parte subito, in parallelo a F5.** In main ci sono già il contratto della rete (`docs/design/network-contract.md`), i driver e100, rtl8139, ne2000, e1000, pcnet, tulip, 3c90x e rhine e l'icona di rete; manca l'import di lwIP con socket, DHCP e DNS, il ponte NE2000/IPX per le VM DOS, Winsock e DirectPlay, TLS e il port di NetSurf.
4. **QEMU torna disponibile** per sviluppare e provare la rete (F6) e lo strato OpenGL (F7), e per il breve smoke di avvio prima di un'immagine hardware; il T23 resta la conferma finale.
5. **Win16 (f5-19b–e) dopo la 1.0 Beta.**
6. **Hardware di prova:** per ora solo il T23 è in rete; il Compaq è stato dismesso e il proprietario si procurerà un secondo PC dell'epoca.

## Riferimenti

- `AGENTS.md` (sezione Testing, aggiornata); `docs/design/network-contract.md`; `docs/design/f7-3d-acceleration-contract.md`; voce 2026-10-20-03 (segmenti a 32 bit di Win16, ora dopo la beta).
