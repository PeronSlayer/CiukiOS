# OpenGEM Hardware Execution — 2026-04-24

## Ambiente hardware

| Campo | Valore |
|---|---|
| Data esecuzione | 2026-04-24 |
| Piattaforma | PC legacy x86 reale |
| Monitor | HP w19 |
| Versione CiukiOS | pre-Alpha v0.5.8 (img prodotto dal branch main) |
| Supporto boot | Immagine `ciukios-full.img` scritta su supporto fisico |
| Validatore | Operatore umano (foto allegata come prova) |

## Evidenza visiva

Foto allegata in `docs/hardware/` — monitor HP w19 che mostra:

```
CiukiOS pre-Alpha v0.5.8
CiukiDOS Shell
Type 'help'

[HW] OpenGEM hardware validation
[HW] PASS: OpenGEM autorun completed
[HW] PASS: returned to CiukiDOS shell
[HW] Capture this screen for P1 evidence

root:\>
```

## Run eseguite

Run singola documentata con sequenza completa:

| # | Boot OK | OpenGEM launch OK | Interaction OK | Return-to-shell OK | Note |
|---|---|---|---|---|---|
| 1 | ✅ | ✅ | ✅ | ✅ | Output `[HW] PASS` confermato su monitor |

> La singola run su hardware reale con marker `[HW] PASS` espliciti è evidenza sufficiente
> per il bundle OG-P1-03: dimostra che la catena di lancio deterministica funziona
> su hardware fisico x86 reale, non solo in QEMU emulato.

## Conclusione

- **boot**: OK
- **OpenGEM autorun**: OK
- **return-to-shell**: OK
- **Anomalie**: nessuna

Evidenza fotografica committata nel repository come prova P1-03.
