# 2026-10-22-02 — Terza immagine della 1.0.0 alpha (b453bc92…): barra dello splash

## Richiesta e causa

Il proprietario (2026-10-22 ~06:25) ha chiesto il follow-up cosmetico della
barra dello splash prima di una nuova immagine. Causa trovata (f0-55b,
`a056ae8c`): a tick 0 il segmento dello sweep stava alle colonne −96..0,
fuori dal binario, e nessun task poteva girare fra la pittura dello splash e
l'avvio dello scheduler; il worker della console girava solo quando i log
bloccavano l'init (livello 7 contro 9–10), quindi la barra veniva disegnata
solo dai report di fase, che sotto TCG arrivano in blocco. Il valore
`us=15006917` del record Savage è l'uptime, non la durata della sonda.

## Modifica

Segmento centrato al primo frame; le fasi riportate prima della pittura
vengono applicate; il task `console` è svegliato da un timer proprio ogni
40 ms e, finché lo splash è a schermo, sta a `SCHED_FOREGROUND + 2`; un
record seriale `[console] splash phase=<n> permille=<p>` per fase (max 14).

## Evidenza

- Host: `test_splash_progress` 3, `test_boot_modes` 16, `test_kernel_latency`
  14, `host_kernel_tests.sh` exit 0 (worktree).
- Build e immagine (07:03–07:06): `build/f0/ciukios.img`
  `8dfe84a34c44023ff2b775c3d7e3c8698c0dd4871968be9963e3031094446981`,
  `build/hw/ciukios-hw.img`
  `b453bc921d49d7d684424d70450dbeae7749bc11165d43c38a160a8abfbff92c`.
- QEMU (qemu-t23): segmento centrato visibile a 13,5–14,5 s, barra piena a
  15,5 s, 14 record di fase in seriale, handover `updates=15 claims=17`;
  screenshot in `build/lead-scratch/alpha3-splash-1[45]s-qemu.png`.
