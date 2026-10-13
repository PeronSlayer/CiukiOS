# Contratto della personalità Win32 (F5): bozza e decisioni del proprietario; runner a due slot; diagnosi del blocco SuperSavage

- **Data:** 2026-10-13
- **Tipo:** decisioni (design); analisi hardware
- **Versione/commit:** `main` da `8b9295f6`
- **Stato:** contratto F5 in bozza con le decisioni registrate; correzione del driver 2D in corso

## Contratto F5

`docs/design/win32-personality-contract.md` (agente Opus, revisione del lead):
processo CiukiOS con personalità `win32`, loader PE, eccezioni in stile
Windows, memoria a 2 GiB con DLL di sistema a 0x70000000, registro su file di
testo, identità "Windows 98 SE", presentazione video tramite il desktop con
diritto di presentare revocabile, contatore delle prestazioni dal PIT; DLL per
fasi; dieci titoli demo/shareware come set di accettazione; direttive
f5-02…f5-18 (più f5-19 per Win16).

Decisioni del proprietario: opzione C (nucleo scritto da CiukiOS, DLL per gli
installer riusate da una release fissata di Wine, LGPL); Win16 incluso in F5;
sintesi MIDI General MIDI con banco a licenza aperta; socket IPX in F6 per il
gioco in LAN; locale bilingue con inglese predefinito e italiano selezionabile.
Decisioni del lead sulle altre domande registrate in coda al contratto
(DirectX 7.0a fino a F7, presentazione tramite copia del kernel, soglie di
"giocabile", CD audio richiesto, fuso orario di sistema, font Liberation con
FreeType, protezioni anticopia in kernel fuori scope, qualifica a 128 MiB).

## Runner

`runner-01` fuso: slot multipli (`CIUKI_RUNNER_SLOTS`, predefinito 2),
conteggio dei QEMU per scope, memoria richiesta per slot in più, scritture
atomiche del riepilogo; suite diverse possono girare in parallelo (la
potatura per suite non è ancora protetta per la stessa suite su due slot).

## Blocco dell'avvio sul T23 con il driver 2D (immagine 8845b848)

Avvio N e L fermi dopo `[init] flag …`, nessuna riga `[init] framebuffer`.
Revisione indipendente (agente Opus, `wt/savage-review`): tutti i cicli
software del driver terminano, la sorgente di tempo è calibrata, il
blocco è un singolo accesso a registro che non completa; candidato
principale la prima lettura del registro di stato 0x48c60 senza reset del
motore (CR66 bit 1), difetto del chip documentato dal driver X.Org e da
ThinkWiki per i ThinkPad T20–T23. La finestra dei registri del SuperSavage
8c2e è BAR0+0 (512 KiB). Direttiva f1-27 corretta di conseguenza (reset e
sblocco prima della prima lettura, letture diradate, marcatore persistente
di tentativo, inizializzazione tardiva); nel frattempo l'acceleratore è
opzionale a compilazione (`03bf0427`).

## Pacchetto driver Windows del proprietario

1,6 GiB di binari Windows in `third_party/`, ignorati da git e mai inclusi
nell'immagine; i file `.inf` alimentano un database di id hardware
(direttiva f2-43) per il Device Manager e per le priorità dei driver.
