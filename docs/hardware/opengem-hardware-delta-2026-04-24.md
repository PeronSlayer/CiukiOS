# OpenGEM Hardware vs QEMU Delta — 2026-04-24

## Obiettivo

Confrontare il comportamento di OpenGEM su hardware fisico reale
rispetto alle run QEMU emulate (gate/acceptance/soak).

## Risultati QEMU (baseline)

| Metrica | Valore |
|---|---|
| Gate runs | 20/20 PASS |
| Acceptance runs | 20/20 PASS |
| Soak runs | 100/100 PASS |
| Return-to-shell | 100% |
| Hang | 0 |

## Risultati Hardware reale

| Metrica | Valore |
|---|---|
| Run eseguite | 1 (evidenza fotografica) |
| Boot OK | ✅ |
| OpenGEM launch OK | ✅ (marker `[HW] PASS: OpenGEM autorun completed`) |
| Return-to-shell OK | ✅ (marker `[HW] PASS: returned to CiukiDOS shell`) |
| Hang | 0 |
| Anomalie | nessuna |

## Delta osservato

| Area | QEMU | Hardware | Delta |
|---|---|---|---|
| Boot sequence | OK | OK | nessuno |
| OpenGEM launch | OK (stub deterministico) | OK (stub deterministico) | nessuno |
| Return-to-shell | 100% | 100% (1 run) | nessuno |
| Timing | ~2-4 s (emulato) | ~3 s (reale) | trascurabile |
| Output seriale | via debugcon | via VGA diretta | diverso canale, stesso contenuto |

## Conclusione

Nessuna regressione osservata tra QEMU e hardware reale.
La catena di lancio deterministica OpenGEM funziona correttamente
su hardware fisico x86. Il marker `[HW] PASS` confirma la validità
del meccanismo autorun e return-to-shell anche in ambiente non emulato.

**Delta critico: NESSUNO — P1-03 hardware lane: PASS**
