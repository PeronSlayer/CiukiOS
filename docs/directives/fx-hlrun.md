# fx-hlrun: Half-Life (Sierra 1998 CD) runs after its InstallShield 5 install

- **Written:** 2026-10-27 21:35 by the lead. Implementer: Claude agent. Base: main 12641a954 or later (fx-is5boot merged: the original CD installs to completion in the H1 harness, opt-in `CIUKI_IS5_INSTALL=1`).
- **Owner request 2026-10-27:** Half-Life installs and plays.
- **Evidence (fx-is5boot round 2):** the engine's last step starts `hl.exe`, which stops on missing imports: `gdi32!GetBoundsRect` (hl.exe), `kernel32!OpenFileMappingA` and `user32!WindowFromDC` (the GL renderer DLL), and many GDI path/metafile calls (Sierra's uninstall utility, SUTIL32). The CD is mounted read-only at `/run/media/peronslayer/Halflife` on the lead's workstation: read it, never copy or commit its bytes, delete every scratch install tree.

## Scope

1. Run the installed game in the H1 harness (extend the is5boot opt-in install test or add a local-only game test): list every import and every runtime call that fails, implement them following Windows 98 behaviour and Wine's implementation as reference (cite both), in the existing Win32 DLLs (sdk/win32/...). No stubs that return success without doing the work, unless Windows 98 itself does so.
2. Get Half-Life to its main menu and into a level with the software renderer (DirectDraw/GDI path) first, then with the OpenGL renderer through CiukiOS opengl32 (software on the host; the Savage hardware path applies on the T23). Record frames as checksums plus local-only PNGs under the worktree's build/ (never committed) so the lead can look at them.
3. The Sierra utility (SUTIL32) and uninstall entries: implement the missing GDI path/metafile calls they need, or document precisely which ones remain.
4. Host regressions for each new API with synthetic tests; the Win32, installer and GL suites stay green.

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-hlrun` (branch `wt/fx-hlrun`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-hlrun-report.md` in the worktree. Leave changes uncommitted.
