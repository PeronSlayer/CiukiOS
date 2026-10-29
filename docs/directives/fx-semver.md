# fx-semver: Semantic Versioning 2.0.0 for CiukiOS versions, stages and release tags

- **Written:** 2026-10-29 09:50 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `medium`.
- **Worktree:** `/home/peronslayer/Desktop/CiukiOS-wt-fx-semver`, branch `wt/fx-semver` from `main`.
- **Owner decision 2026-10-29** (dev diary 2026-10-29-03):
  - versions follow Semantic Versioning 2.0.0, https://semver.org/spec/v2.0.0.html;
  - the labels that were never published are renamed retroactively;
  - a release-candidate stage is added before the stable 1.0.0;
  - the current beta is `1.0.0-beta.1`.

## The scheme

| Stage | Version | Git tag | GitHub prerelease |
|---|---|---|---|
| Development | `0.y.z` | none, or `v0.y.z` | yes |
| Alpha | `1.0.0-alpha.N` | `v1.0.0-alpha.N` | yes |
| Beta | `1.0.0-beta.N` | `v1.0.0-beta.N` | yes |
| Release candidate | `1.0.0-rc.N` | `v1.0.0-rc.N` | yes |
| Stable | `1.0.0`, then `1.0.1`, `1.1.0`, `2.0.0` | `v1.0.0` | no |

- **Build metadata** follows SemVer §10: `+build.<build id>.g<short commit>`. It never affects ordering and never appears in a tag.
- **Human name.** A stable `X.Y.Z` is shown as `CiukiOS X.Y.Z`. A pre-release `X.Y.Z-<stage>.N` is shown as `CiukiOS X.Y.Z <Stage> N`, for example `CiukiOS 1.0.0 Beta 1`.
- **Public API**, which decides when MAJOR and MINOR change after 1.0.0:
  - the documented native API (`docs/developer-api.md`);
  - the on-disk formats.

  Breaking either needs a new MAJOR.

## Scope

1. **`config/version.json`** carries the full SemVer string, `"version": "1.0.0-beta.1"`. Drop the separate `stage` field, or derive it from the version; do not keep two sources of truth.
2. **`scripts/ciuki_version.py`.**
   - Validate the string with the official SemVer 2.0.0 regular expression from semver.org.
   - Accept only these pre-release forms: `alpha.N`, `beta.N` and `rc.N`, with N ≥ 1 and no leading zero.
   - Return core, stage, number, the human name and the build-metadata form.
   - Add a host-tested precedence helper following SemVer §11.
3. **Build and runtime.** Update:
   - `scripts/build_kernel.py`, `scripts/build_image.py` and the build manifest: record the SemVer string and the `+build` form;
   - `src/kernel/include/ciuki/version.h`, `core/main.c` and `uname` (`proc/syscalls_file.c`): the `version` field gets the SemVer string, and `release` stays the build id;
   - the loader menu and startup screens (`src/boot/ciukldr*`): the human name, within `scripts/loader_budget.py`;
   - the self-test report;
   - the desktop About window: the human name, plus the full `+build` string in its details;
   - Setup and the CD media.

   The kernel banner parsed by `scripts/test/physical.py` stays unchanged.
4. **`scripts/release_stable.py`.**
   - The tag is `v<version>` with no suffix counter.
   - The GitHub prerelease flag is set exactly when the version has a pre-release part.
   - Remove `--channel`. Derive the channel from the version and refuse a tag that already exists, locally or remotely.
   - The stable owner approval stays as it is. Approval is required for every channel.
   - The release notes file is chosen by stage. Rename `docs/release-notes-1.0-alpha.md` and `docs/release-notes-1.0-beta.md` only if the scheme needs it, and update every link.
   - Update `docs/release-process.md`.
5. **CHANGELOG.**
   - Put a "Versioning" section at the top: the scheme above and the mapping table below.
   - Rename the unreleased headings, adding "(formerly labelled …)":
     - `1.0.2/1.0.1 beta` → `1.0.0-beta.1`;
     - `1.0.1 alpha` → `1.0.0-alpha.2`;
     - every "1.0.0 alpha" image heading → `1.0.0-alpha.1`;
     - `0.9.2 pre-alpha` → `0.9.2`;
     - `0.9.0 pre-alpha` → `0.9.0`.
   - Do not change the 0.8 line's published identifiers.
   - Align `README.md`, `VERSION`, `docs/build-and-run.md`, `docs/desktop.md`, `docs/design/execution-abi.md` and the release notes. Keep existing folder names such as `docs/screenshots/1.0.1-alpha/`.

   | Former label | Published | New identity |
   |---|---|---|
   | `v0.8.3-…-b847`, `v0.8.3-…-b849` (branch `legacy-0.8`) | yes, GitHub prereleases | unchanged; recorded as `0.8.3+b847…`, `0.8.3+b849.g868cac9e` |
   | 0.9.0 pre-alpha | no | `0.9.0` |
   | 0.9.2 pre-alpha | no | `0.9.2` |
   | 1.0.0 alpha (hardware images of 2026-10-21/22) | no | `1.0.0-alpha.1` (images told apart by `+build`) |
   | 1.0.1 alpha (2026-10-25) | no | `1.0.0-alpha.2` |
   | 1.0.1 beta (2026-10-29) | no | `1.0.0-beta.1` |
6. **Tests.** Update or add host tests for:
   - validation, including rejection of `1.0.0-pre-alpha`, `01.0.0` and `1.0.0-beta.01`;
   - precedence;
   - the human name;
   - manifest fields;
   - the loader budget;
   - release tag and prerelease-flag selection;
   - changelog section lookup by full version;
   - the PC-model public-text check on the renamed sections.

   Run them, `tests/host/test_version.py`, `tests/host/test_release_stable.py`, `tests/host/test_boot_modes.py`, `tests/host/test_startup_menu.py`, `scripts/test/cd_media_tests.py` and `python3 scripts/gen_api_reference.py --check`, sequentially with `nice -n 19`.

## Rules

- Never commit or stage. Never run QEMU, `systemd-run`, image builds, `gh` or `git tag`.
- Do not edit `dev_diary/` entries or `config/release-policy.json`.
- Write the report to `build/codex/fx-semver-report.md`.
