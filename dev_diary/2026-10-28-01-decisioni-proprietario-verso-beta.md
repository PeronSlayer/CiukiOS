# Decisioni del proprietario per il lavoro continuo verso la 1.0 Beta

- **Data:** 2026-10-28 12:12
- **Tipo:** decisione

## Decisioni

Il proprietario ha chiesto di proseguire senza interruzioni fino alla 1.0 Beta e ha risposto alle domande aperte:

1. **QEMU per i test di rete:** fino a tre QEMU contemporanee, solo per i test LAN (IPX/SPX, DirectPlay, giochi DOS in rete). Ciascuna resta nel proprio scope con `MemoryMax=1500M` e `MemorySwapMax=0`. Per tutti gli altri test resta la regola di una QEMU alla volta.
2. **Agenti Claude:** fino a tre in parallelo; Codex resta il canale principale di implementazione.
3. **Rilascio:** quando l'immagine Beta supera su QEMU la suite di accettazione `acc-t23` e l'autotest, si pubblica subito una prerelease 1.0 Beta su GitHub, senza attendere la prova su hardware. Prima della pubblicazione:
   - si aggiorna `config/release-policy.json`;
   - si soddisfano le condizioni di licenza che vi sono registrate: sorgenti e avvisi Wine e offerta del sorgente.
4. **NetSurf:** JavaScript (Duktape) acceso di default nella Beta.
5. **Backend 3D delle altre schede video** (ATI Radeon e Mach64, Matrox G200/G400, NVIDIA Riva/GeForce, VMware SVGA): entrano nella Beta, con priorità inferiore a giochi, LAN e accettazione.
6. **Half-Life:** per l'installazione nell'harness si usa la chiave generica indicata dal proprietario, non la sua chiave originale. Il file della chiave e l'ISO privata si cancellano a fine lavoro.
7. **Giochi DOS della Beta:** nell'immagine e nei test solo shareware, demo e freeware ridistribuibili. I titoli commerciali dell'elenco privato li prova il proprietario.

## Conseguenze

- La coda Codex mette LAN (f6-25, f6-26) prima dei backend 3D (f7-24, f7-25, f7-26, f7-27, f7-31).
- La preparazione del rilascio comprende il pacchetto dei sorgenti Wine e l'offerta del sorgente.
