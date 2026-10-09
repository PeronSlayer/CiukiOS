# Desktop nativo, monitor V86 e più VM DOS

- **Data:** 2026-09-26 – 2026-09-29
- **Tipo:** retrospettiva
- **Versione/commit:** `c36be36`, `abbffb0`, `e81f01b`, `92f4c86`, `7952df8`, `d6659ee`
- **Stato:** completato su QEMU; hardware reale non qualificato

## Cosa è stato fatto

- **Desktop nativo** dentro SHELL.COM: finestre DOS in testo BIOS, file
  manager grafico, sfondi, suoni di sistema.
- **Fondamenta V86 e DPMI**: JemmEx con patch come monitor V86, modulo JLM
  **CVSESSION** per le sessioni (VGA privata, trasporto framebuffer protetto),
  adattatori per HDPMI.
- Dispositivi del guest: SB16, OPL, AC'97, tastiera e INT 33h virtuali;
  DOS/4GW (DOOM originale e doom-vanille) dentro la finestra.
- **Multi-VM M1–M3**: più VM DOS create per fork, scheduling preemptivo,
  I/O concorrente, sessioni video/input/audio separate.
- App del desktop (Files, Notepad, Task Manager) come moduli C `.APP`.

## Lezioni

- Stack extra globale di SeaBIOS: due VM in attesa in INT 16h si scambiavano
  i registri. Corretto rendendo private per VM le pagine RAM del BIOS.
- La creazione di una VM tramite fork è l'unico modo coerente con questo
  kernel, perché lo stato di allocatore ed EXEC vive accanto agli MCB.
  Questa scelta porta però stato condiviso tra padre e VM (vedi
  [revisione architettura](2026-10-09-02-revisione-architettura.md)).

## Riferimenti

- `docs/dos-window-architecture-2026-09-26.md`
- `docs/design-multi-vm-2026-09-28.md`
- `docs/history/september-27-29-increments.md`
