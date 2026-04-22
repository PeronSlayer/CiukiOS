# 2026-04-19 — OPENGEM-004: App Discovery and File Catalog

## Context and goal
Phase 4 of the OpenGEM UX roadmap
([docs/roadmap-opengem-ux.md](../roadmap-opengem-ux.md)). Replace the
ad-hoc "demo COM only" discovery with a single, de-duplicated app
catalog populated from (1) a FAT scan of the well-known stage2 roots
and (2) the loader-provided `handoff->com_entries[]`. Expose it
through a shell command and a stable internal ABI. No version bump
(`CiukiOS Alpha v0.8.7`). No changes to `main`.

## Files touched
- `stage2/include/app_catalog.h` — new module contract.
- `stage2/src/app_catalog.c` — scan + dedupe + list/find API.
- `stage2/src/stage2.c` — includes app_catalog.h and calls
  `app_catalog_init(handoff)` after FAT mount.
- `stage2/src/shell.c` — includes app_catalog.h, adds
  `shell_cmd_catalog()` and the `catalog` dispatch; `help` now
  advertises the new command.
- `scripts/test_opengem_file_browser.sh` — new host-side static
  smoke gate (37 assertions) + opt-in boot-log probe.
- `Makefile` — new target `test-opengem-file-browser`.
- `docs/roadmap-opengem-ux.md` — Phase 4 status flipped to DONE.
- `documentation.md` — new item 15 in Current Project State.
- `docs/handoffs/2026-04-19-opengem-004-catalog.md` — this file.
- `docs/collab/diario-di-bordo.md` — local diary entry (gitignored).

## Decisions made
- **Append-only entry shape.** `app_catalog_entry_t` is
  `{char name[13]; char path[64]; u8 kind; u8 source; u8 reserved[2];}`.
  `reserved[2]` is a future hook (e.g. `size_hint`) without a
  struct-layout break.
- **Static backing storage.** `g_entries[256]` + `g_count`. No
  dynamic allocation. Bounded memory cost: ~21 KiB, comfortable
  inside stage2's data segment.
- **Two-lane dedupe with FAT-wins tie-break.** A user can override a
  bundled demo by dropping a `.COM` on the image at `/FOO.COM` and
  the FAT lane's entry will be registered first, so the handoff lane
  will skip on collision. This matches the policy the roadmap calls
  out.
- **Scan roots are hardcoded.** `/`, `/FREEDOS`, `/FREEDOS/OPENGEM`,
  `/EFI/CiukiOS`. Adding a new root is a one-line change to
  `k_scan_roots[]`; keeping it static avoids a runtime parser and a
  config source.
- **FAT-ready fallback.** When `fat_ready() == 0` the module skips
  the FAT lane and populates only the handoff lane, emitting
  `[ catalog ] fat not ready, skipping FAT scan`. Stage2 continues
  to boot.
- **Shell command is list-only.** `catalog` prints name, kind tag
  and path for each entry. Filtering and pattern matching are a
  future extension; current UX parity is "a usable inventory."
- **Services ABI extension deferred.** Exposing
  `ciuki_services_t.app_catalog` requires a careful append-only
  tail field with a null guard for legacy callers. Stage2's
  catalog is already accessible from the shell (the primary
  consumer in Phase 4). A dedicated micro-task will wire the
  services ABI once a real COM (GEMVDI host-app) needs it.
- **PATH resolver extension deferred.** The existing command
  dispatch already falls back to FAT lookup via
  `shell_run_from_fat()`, so discovery-by-name for `.COM` and
  `.EXE` already works. BAT dispatch via catalog entry is a
  marginal benefit — FreeDOS and OpenGEM batches live at stable
  paths. Will revisit if a real BAT fails to resolve.
- **`[G]` glyph from Phase 3 unchanged.** The catalog does not
  alter the dock rendering.

## Validation performed
- `make test-opengem-file-browser` → **PASS** (37 OK / 0 FAIL).
- `make test-opengem-launch` → **PASS** (Phase 3 regression).
- `make test-bat-interp` → **PASS** (Phase 2 regression).
- `make test-opengem-smoke` → **PASS**.
- `make test-opengem` → **PASS** (help contract includes `catalog`
  line — the gate greps for the OPENGEM substring which stays
  intact).
- `make test-gui-desktop` → **PASS**.
- `make test-mouse-smoke` → **PASS** (static fallback).
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` → **PASS**.

`make test-stage2` / `make test-fallback` remain blocked on macOS
per `/memories/repo/ciukios-build-notes.md`; not introduced by this
task.

## Risks and next step
- **Services ABI extension deferred.** Documented as follow-up.
  Risk: a future DOS-native OpenGEM host-app cannot iterate the
  catalog via the stable ABI. Mitigation: add a tail field
  `ciuki_services_t.app_catalog_list(entry *, uint32_t cap)` the
  moment a concrete consumer lands; the append-only rule holds.
- **PATH resolver extension deferred.** Risk: BAT files not in the
  current directory or a hardcoded probe list may not dispatch.
  Mitigation: existing FAT fallback already covers the vast
  majority of cases; add a catalog probe if a real regression
  surfaces.
- **Dedup key is pure 8.3 name.** Two files with identical names in
  different scan roots collapse into one entry. For our 4 well-known
  roots that is desirable; if the roots list ever grows into
  user-content territory, path-scoped dedupe may be preferable.
- **Scan cost bounded by depth=1.** We scan each root once; we do
  not recurse into subdirectories. FreeDOS bundle and OpenGEM sub-
  trees will be under-counted for the `catalog` UX view. Adding a
  single level of recursion is trivial when Phase 6 (DOOM) needs
  discovery under `/GAMES/`.

### Next phase inputs
- Phase 5 (OPENGEM-005 Input/Mouse) does not depend on the catalog
  directly but benefits from deterministic program availability
  while routing through OpenGEM.
- Phase 6 (OPENGEM-006 DOOM) will reuse `app_catalog_find("DOOM.EXE")`
  to emit `[ doom ] catalog discovered DOOM.EXE at <path>` and
  gate on it. The one-level-deep scan is sufficient once DOOM is
  staged at `/GAMES/DOOM/DOOM.EXE` because `k_scan_roots` can
  include `/GAMES/DOOM` on an opt-in flag.
