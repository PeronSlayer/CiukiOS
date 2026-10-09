# SHELL.COM, kernel CIUKIDOS separato e compatibilità DOS

- **Data:** 2026-05-18 – 2026-09-01
- **Tipo:** retrospettiva
- **Versione/commit:** pre-Alpha 0.6.7, 0.7.1 (`1dd520a`)
- **Stato:** completato nel perimetro registrato

## Cosa è stato fatto

- La shell passa a **SHELL.COM** esterno; Stage1 diventa solo loader
  (1.542 byte).
- Nasce il kernel **CIUKIDOS.SYS**, residente e con interfaccia versionata
  (ABI 2, 11 servizi), tetto di 43.264 byte (Fase 5 chiusa).
- Servizio mouse INT 33h su PS/2, MOUSE.COM, spegnimento reale, splash con la
  foto di Ciuki.
- Lavoro esteso su WOLF3D: JFT dei figli, loader MZ per dati inizializzati
  grandi, servizi VBE con bank switching.
- 21 luglio: regressione di DOOM causata da TF che trapelava da un `popf`
  nell'allocatore; corretta.
- 0.7.1: rete IPv4/ICMP/FTP residente, Costa e DOSNavigator,
  **Windows 3.1 in modalità 386 avanzata** in un perimetro limitato,
  profili VGA-fast, audio OPL2/SB16/PC speaker su QEMU.

## Riferimenti

- `docs/history/changelog-v0.7.1-2026-09-01.md`
- `docs/shell-com-migration-2026-05-17.md`
- `docs/diario-bordo-v2.md`, voci 104–123
