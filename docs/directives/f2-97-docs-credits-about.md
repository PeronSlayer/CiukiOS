# f2-97 — Documentation and credits up to date; readable credits in About CiukiOS

- **Phase/model:** F2/docs, Claude agent (Sonnet). **Worktree:** `wt/f2-97` from `main`.
- **Owner request (2026-10-22 18:40):** after the current fixes, update the documentation and the credits, and fix the credits in the system's About dialog, which are not very readable.

## Required

1. **Credits inventory:** one authoritative list of third-party components shipped in the image or used to build it, with name, version/pin, licence and the notice path: everything under `third_party/`, `assets/icons` (Tango), the pinned downloads (`config/*.json`: FreeDOS 1.3 and the 2043 fork, Open Watcom 1.9, Wine 10.0 reused DLLs, lwIP 2.2.1, NetSurf 3.11 and its libraries, BearSSL, jitterentropy-library 3.7.0, stb, minimp3, TinySoundFont, tjpgd, tcc, Lua, DOOM/doomgeneric, Wolf3D/wolf4sdl, DOS Navigator, Jemm, CTMouse, vsbhda, Microsoft/Intel driver packs if redistributed), plus the pixman / X11 region code being added by f2-95 (MIT). Record it in `docs/credits.md` (English), generated or checked by a host test against `config/` pins and `third_party/*/LICENSE*` so it cannot drift.
2. **About CiukiOS dialog** (`apps/desktop/shell.c` DESK_INTERNAL_ABOUT and its renderer): the credits become a scrollable, legible page — Ciuki portrait and identity at the top (tagline `Classic computing, one woof at a time.`), version from `config/version.json`, then the credits list in a readable font size with one line per component (name, version, licence), grouped (System, DOS, Windows compatibility, Network and browser, Media, Tools, Icons), with a "Licences" entry that opens the notices folder in Files; keyboard and wheel scrolling; golden frame test; no Tango icon or portrait changes.
3. **Documentation refresh:** `README.md` (what the 1.0.0 alpha contains today, the T23 status, the unified DOS kernel default, the credits pointer), `docs/desktop.md`, `docs/build-and-run.md` (ecpoll tokens, the accel indicator, the splash bar), `docs/release-notes-1.0-alpha.md` aligned with the dev diary entries of 2026-10-21/22; CHANGELOG untouched (the lead keeps it).
4. Host tests: the credits consistency test; the About golden frame; `test_version`.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f2-97-report.md`.
