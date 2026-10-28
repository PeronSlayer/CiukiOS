# fx-games20b: licence terms of the fourteen newly pinned DOS titles

- **Written:** 2026-10-28 19:07 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `medium`. Base: main 61e738370 or later.
- **Inputs:**
  - the lead has downloaded and pinned the fourteen candidate archives (commit 5125ccb2a). They are in `/home/peronslayer/Desktop/CiukiOS/build/downloads/dos-titles/`, which is read-only for you;
  - `config/dos-titles.json`;
  - `third_party/games/*/UPSTREAM.json`;
  - `docs/validation/fx-games20.md`;
  - the fx-games20 report.
- **Mirror names.** The lead corrected seven 3D Realms names from the mirror index:

  | Title | Archive |
  | --- | --- |
  | Shadow Warrior | `3dsw12.zip` |
  | Duke Nukem II | `4duke.zip` |
  | Raptor | `1rap12.zip` |
  | Hocus Pocus | `1hp11.zip` |
  | Bio Menace | `1bio11.zip` |
  | Blake Stone | `1bs30.zip` |
  | Terminal Velocity | `3dtv12.zip` |

## Scope

1. **Read each licence from the pinned package.** For each of the fourteen titles (heretic, hexen, descent, descent2, shadowwarrior, wacky, cosmo, duke2, raptor, hocus, biomenace, blakestone, terminal, jazz), read the licence and vendor documents inside the exact pinned archive. Read inside Apogee `.SHR` members with the existing `shr_members` support in `scripts/test/dos_titles.py`. Record:
   - `licence.source` as `archive!member[!member]`, as for rott/keen4/duke3d;
   - `licence.sha256` of the licence member;
   - `licence.terms`, a short exact summary of the sharing and bundling clauses;
   - `licence.verification`.

   Record the same facts in the title's `UPSTREAM.json`.
2. **Check the title definitions against the package.** If a version or edition differs, for example Bio Menace 1.1 or Blake Stone 3.0, correct the title definition:
   - executable names;
   - installer type;
   - version text.

   Never guess: read it from the archive member list or `FILE_ID.DIZ`.
3. **Class.**
   - Every title stays `test-media` unless its own terms grant redistribution inside an operating-system image without separate permission.
   - Apogee/3D Realms VENDOR.DOC section 3C requires written permission for bundles.
   - Report any title whose terms allow bundling. Do not change its class; the lead decides.
4. **Installed trees.** Say which titles can get an installed tree without running their DOS installer, and which need the lead's QEMU installation. Do not fabricate installed trees.

## Acceptance

- `python3 -B scripts/test/dos_games.py validate` passes.
- `tests/host/test_dos_games20.py`, `tests/host/test_dos_titles.py` and `tests/host/test_acceptance.py` pass.
- No game bytes enter the repository.

## Rules

- Work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-games20b` (branch `wt/fx-games20b`).
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Report to `build/codex/fx-games20b-report.md`. Leave the changes uncommitted.
