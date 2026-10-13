# f2-47 — System registry: Windows-style key tree stored as readable text, with a Registry Editor

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-47` from `main`. Host-testable. Owner decision 2026-10-13: hierarchical keys like Windows 9x, but text files on disk (the `.reg` export format becomes the storage format), a volatile Hardware root, a graphical editor; no binary hives.

## Phase 1 — contract (`docs/design/registry-contract.md`, written and self-reviewed against the code before Phase 2)

- **Namespace**: roots `System`, `Desktop`, `Programs`, `Users`, `Hardware` (volatile, read-only, generated at boot from the devices API `ciuki/devices.h` / the PCI inventory of f2-41 Device Manager). Keys `Root\Sub\Key`, case-insensitive, UTF-8 names ≤ 64 chars, depth ≤ 16; values typed: string, int32, bool, binary (hex). A key has values and subkeys.
- **Storage**: one text file per persistent root in `/System/Registry/<Root>.reg` on the settings volume; format: `[Root\Sub\Key]` sections, `name = "string"`, `name = 123`, `name = true`, `name = hex:00ff…`, `;` comments preserved on rewrite where feasible; atomic write (temporary + fsync + rename) with the journal pattern of `apps/desktop/settings_io.c`; a `generation` counter per root for change detection.
- **Library** `libreg` in the SDK (`sdk/`, header `ciuki/registry.h`): `reg_open_root`, `reg_get_*`, `reg_set_*`, `reg_delete`, `reg_enum_keys/values`, `reg_export(key, path)`, `reg_import(path)`, `reg_generation(root)` for polling-based watch. Concurrency: last writer wins per root with the generation check; a process-wide lock is not available, so document the rule (one writer per root at a time, the Registry Editor and the owning app).
- **Hardware root** content: `Hardware\PCI\<bus:dev.fn>` with vendor, device, class, driver, state, resources; `Hardware\Display`, `Hardware\Audio`, `Hardware\Network` summaries from the existing nodes.
- **Migration policy**: existing settings files (desktop settings, the audio persistence) are not moved in this directive; the contract names them and the later step that migrates them.

## Phase 2 — implementation

- `libreg` with host tests (parse/serialise round trips, atomic replacement, journal recovery, limits, generation).
- **Registry Editor** `/Programs/regedit` ("Registry Editor" under Programs > System tools or Accessories as the catalog allows): tree on the left, values on the right, search, edit with confirmation, new key/value, delete with confirmation, export a branch, import a file; Hardware root read-only.
- Build integration: `scripts/build_image.py` seeds `/System/Registry/System.reg` with `System\Version` (name, version, build hash) and `System\Identity\Tagline`; payload inventory tests updated.

## Acceptance

Host tests for the library and the editor through the desktop harness; `make CAP= apps` builds; developer API docs regenerated; the contract document complete with the decisions and the migration step named.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f2-47-report.md`.
