# fx-hlutils: the Win32 API gaps of Half-Life's bundled Sierra utilities

- **Written:** 2026-10-28 20:18 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`. Base: branch `wt/integ8`, which contains fx-hlrun.
- **Inputs:**
  - `../CiukiOS-wt-fx-hlrun/build/codex/fx-hlrun-report.md`, "Still open" and its lists of missing imports;
  - `config/wine-upper.json`;
  - `sdk/win32`;
  - the Wine 10.0 sources pinned in `config/wine-pin.json`.

## Scope

The Half-Life CD's Sierra utilities fail to load:

- SUTIL32 and SIGSPat import `oledlg.dll`;
- SIGSPat also imports `wininet.dll`;
- UtDel32 needs about 45 gdi32 path and metafile functions;
- the registration DLL needs 5 serial-port and 2 joystick functions.

Make every import of these executables resolve with real behaviour. These are general Win32 gaps that other period programs also hit.

1. **`oledlg.dll` and `wininet.dll`.** Reuse the Wine 10.0 PEs through the existing upper-store recipe where Wine has them, as for the other sixteen reused PEs. Update `config/wine-upper.json`, `third_party/wine/patches/` and the source-offer records.
   - wininet's network paths use the CiukiOS Winsock.
   - Any HTTP/FTP behaviour that is not available returns the documented error codes.
2. **gdi32 paths and metafiles.** `BeginPath`/`EndPath`/`StrokePath`/`FillPath`/`PathToRegion`, the enhanced and Windows metafile record/playback set the report lists, and the related calls.
   - Implement them in the CiukiOS GDI, following Wine's semantics and the Microsoft documentation.
   - Add pixel tests.
3. **Serial and joystick.**
   - `kernel32` comm functions (`SetCommState`, `GetCommState`, `SetCommTimeouts`, `EscapeCommFunction`, and the others the report names) on the CiukiOS serial ports.
   - `winmm` `joyGetPosEx` and `joyGetDevCaps` on the CiukiOS game-port/USB joystick path.
4. **Census.** Regenerate the census and the builtin/upper inventories with their scripts.

## Acceptance

- New host tests for each area pass.
- The Win32 suite gains no failure.
- `python3 scripts/gen_api_reference.py --check` passes.
- The import census of the four Sierra executables, from the lead's private CD, shows no unresolved import. The lead runs it; do not access the CD yourself.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-hlutils` (branch `wt/fx-hlutils`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`.
- Report to `build/codex/fx-hlutils-report.md`. Leave the changes uncommitted.
