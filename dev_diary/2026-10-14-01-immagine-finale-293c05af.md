# Immagine finale dell'ondata del 13 ottobre: 293c05af (ventitré consegne fuse)

- Data: 2026-10-14
- Tipo: modifiche (kernel, driver, desktop, SDK, test, runner), immagine
- Base: main `59d66440`; immagine QEMU `build/f0/ciukios.img` `9390ed8f…`; immagine T23 `build/hw/ciukios-hw.img` SHA-256 `293c05af80fe12c5c7c4c307d7e02e9467d9c1cd2c0eab2536bd4e8092c2c85d`

## Cosa contiene oltre all'immagine 24c98799 del 13 ottobre

Le tredici consegne di `2026-10-13-06` (USB, UDMA, NIC, audio, Terminal, Registry, font, compositor, demo, AutoPlay, barra, DOS Box, suoni) più le dieci dell'ondata serale (`2026-10-13-07`: f1-47 fase dati USB storage, f1-48 lettore ottico, f1-49 Savage via porte, f2-50 architettura di rendering, f2-51 AutoPlay, f2-52 demo grafica, f2-53 barra, f3-13 DOS Box, f4-12 suoni, f4-13 audio delle VM) e le correzioni della notte: f1-45 (task USB mai avviato), f1-46 (input senza i8042), f1-50 (TD periodici USB scollegati prima del frame), f1-51 (decoder tastiera), f2-54, f2-55 (impronta di memoria del desktop deterministica), f2-56 (shell attiva dal primo fotogramma in modalità sonda), f2-57 (harness del gate senza numeri di descrittore fissi); CLOEXEC sulle aperture persistenti (icone, cache sfondo, libreg); il runner attende il ritratto Ciuki prima della schermata pre-input e il prerequisito host ha 40 minuti.

## Verifiche sull'immagine

Suite desktop 92/92, gate 10/10, discovery completa `tests/host` 530 test, build-full con albero pulito, test host del kernel e del filesystem, payload 8/8, f0-smoke 4/4, caso `desktop-shell-qemu-t23` su QEMU passato (sonda a 100 cicli con ledger uguali). Suite `f1-usb`: avvio con soli dispositivi USB (i8042 spento), tastiera e mouse enumerati, mouse dietro l'hub funzionante, tasti tradotti e consegnati (`[input] usb key usage=45 set1=058`); il caso resta rosso perché le fasi dello shell test non si completano con l'input USB (`keys=0` fino allo stadio 6): direttiva f2-58.

## Risultati attesi sul T23

Savage: percorso a porte e sonda dell'engine su MM48C18 (`[accel] savage … engine_probe`); chiavetta USB con budget bulk corretto; lettore ottico legato con disco inserito; audio nativo stabile (passaggio AC97 e seriale in coda) e audio delle VM DOS; DOS Box con finestra di caricamento; suoni di sistema; AutoPlay; Terminal, Registry Editor, demo grafica.
