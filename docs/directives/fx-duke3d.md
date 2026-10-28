# fx-duke3d: Duke Nukem 3D shareware among the pre-installed demo games

- **Written:** 2026-10-26 16:47 by the lead. Implementer: Codex gpt-6.1-sol, effort high. Base: main be7852468.
- **Owner request:** add the Duke Nukem 3D demo among the games.

## Scope
1. **Source and licence.**
   - Use the official 3D Realms shareware release, v1.3D (3dduke13.zip / DN3DSW13.SHR), from a stable primary source.
   - Record its URL and SHA-256 and pin it like the other games in the repository's game catalogue and payload scripts (see how the DOOM, Wolfenstein 3-D, Quake shareware and the Warcraft II demo are fetched, verified and installed).
   - Read the shareware licence text in the archive (LICENSE.DOC/VENDOR.DOC), quote the redistribution terms in the report, and keep the licence file installed with the game.
   - Never include registered or full-version files.
2. **Install** it in Programs > Games like the other demos:
   - menu entry, icon from the game if it has one;
   - launcher batch, DOS/4GW;
   - DUKE3D.CFG preset for Sound Blaster 16 (A220 I5 D1 H5), music on the emulated OPL/MIDI that CiukiOS supports, 320x200 or a VESA mode that works in the window;
   - game-data save handling like the other games.
3. **Host checks.**
   - Payload hash.
   - Catalogue and menu tests.
   - The DOS/4GW start through the existing extender host replay, like Warcraft II's DOSX test, as far as the host harness allows.

   Write the exact QEMU check the lead will run: menu launch, title screen, start a game, sound, exit.

Report to build/codex/fx-duke3d-report.md. Leave changes uncommitted.
