# fx-beta-prep: version, notes and changelog for the 1.0 Beta prerelease

- **Written:** 2026-10-29 09:24 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-beta-prep`, branch `wt/fx-beta-prep` from `main`.
- **Owner decision:** dev diary 2026-10-28-01. The 1.0 Beta prerelease is published once `acc-t23` and the self-test are green on QEMU.
- **Lead decision:** the beta is version `1.0.2`, stage `beta`. `release_stable.py --channel beta` will tag it `v1.0.2-beta.<n>`. The unpublished `1.0.1 alpha` section stays as written.

## Scope

1. **Version.** Set `config/version.json` to `1.0.2`, stage `beta`. Update `VERSION`, `README.md` and `CHANGELOG.md` so that `tests/host/test_version.py` passes. Do not edit `config/release-policy.json`: the lead sets the approval fields at release time with the qualified image SHA-256.
2. **CHANGELOG.** Add a section `## 1.0.2 beta — unreleased` covering everything merged since the 1.0.1 alpha section. Use `git log` and the `dev_diary/` entries since 2026-10-25 as sources.
   - Group the entries: user security and accounts, Win32 and Wine components, 3D/OpenGL, networking and LAN, DOS games and sound, Half-Life runtime, desktop and Files, Setup CD.
   - Facts only, with no internal agent names.
   - Describe hardware generically (late-1990s/early-2000s desktops and laptops, component classes). Never name a PC model.
3. **Release notes.** Write `docs/release-notes-1.0-beta.md`, modelled on `docs/release-notes-1.0-alpha.md`. It must cover:
   - what the beta contains and how to install it (Setup CD, raw disk, VM images);
   - known limitations;
   - the LGPL source offer for the reused Wine 10.0 components;
   - the rule that commercial game data is never included (shareware, demo and freeware only).

   Extend the host test that rejects PC model names to scan this file too.
4. **Packaging.** Make `scripts/release_stable.py` pick the release notes for the channel: beta notes for `--channel beta`, alpha notes for alpha. Keep the stable behaviour. Add host tests for the selection, and check that a beta run with stage `beta` passes the stage/channel check while a mismatch is refused. Do not invoke `gh` or network in tests.
5. Run `tests/host/test_version.py`, the release script's host tests and `python3 scripts/gen_api_reference.py --check` sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run`, image builds or `gh`.
- Never read or quote `legacy/local/`. The private 1.0 Beta title and site lists never enter tracked files.
- Write the report to `build/codex/fx-beta-prep-report.md`.
