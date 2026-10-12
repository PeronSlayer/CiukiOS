# f2-35 — Retro namespace: Programs, Documents, System and Games on C:\ (owner decision 2026-10-12)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-35` (branch `wt/f2-35` from `wt/integ2` f1cedd38). Host-testable code only; the lead runs QEMU (f0-smoke, f2-desktop, f2-process, f3-vm) and the hardware.
- **Runs in parallel with f2-36** (Files "Computer" view in `../CiukiOS-wt-f2-36`): do not redesign Files here; only rename its path literals.

## Decision (owner, 2026-10-12)

The user-visible file tree follows Windows 98/ME and Mac OS 9, not Unix. The POSIX view over the drive VFS stays as the contract defines it (`docs/design/posix-subset.md`, "POSIX paths over the drive VFS": `/` = `C:\`, `/mnt/d..z`, synthetic `/dev`), but the ordinary boot-volume directories change name:

| Before | After | Notes |
| --- | --- | --- |
| `/bin` | `/Programs` | all native programs; `PATH=/Programs` |
| `/home` | `/Documents` | `HOME=/Documents`; recent list `/Documents/recent.lst` |
| `/system` | `/system` | unchanged in code; created on disk with the long name `System` (paths are case-insensitive and case-preserving, `docs/design/vfs-storage-contract.md` line 60), so Files shows "System" |
| `/GAMES` | `/Games` on disk | the kernel keeps `C:\GAMES\CIUKIGAM.IMG` (`src/kernel/vm/dosvm_disk.c`, case-insensitive lookup) |
| `/tmp` | `/tmp` | unchanged; Files hides it (f2-36) |

`/dev` and `/mnt` remain synthetic structural nodes, unchanged.

## Scope

1. **Contract.** Amend the F2 clause "`/bin`, `/tmp`, `/home` and `/system` MUST be ordinary boot-volume directories created by the canonical image builder" in `docs/design/posix-subset.md` with the table above and the display rule (long names `Programs`, `Documents`, `System`, `Games`), dated, citing the owner decision; do not rewrite earlier sections. Update `docs/design/f2-acceptance.md` where it names these paths.
2. **Image builder.** `scripts/build_image.py`: `directories`, `sources` (`/Programs/<name>`, `/Documents`, `System` and `Games` long names), T1 checks, manifest; `scripts/build_games_disk.py` only if it names the folder; `tests/host/test_image_payloads.py`, `tests/host/desktop/test_desktop.py` inventories and FAT read-back.
3. **Kernel.** Supervisor bootstrap path (`spawning /Programs/desktop`, `desktop pid=` milestone unchanged), the F2 probes' records (`path=/Programs/desktop clients=/Programs/demo` and any other `/bin` literal), the gate metadata env (`PATH`, `HOME`), workload or fixture paths. Keep the record grammar (`[A-Za-z0-9_:.,/+-]`).
4. **SDK and libc glue.** Default `PATH`/`HOME` wherever set (`src/sdk`, `apps/libdeskui/recent.c`), exec/lookup helpers, help texts naming `/bin` or `/home`.
5. **Apps.** `apps/desktop/catalog.h`/`shell.c` launch paths and the Run dialog help text ("program in /Programs"), desktop icon "Documents" → `/Documents`, `apps/accessories/help_topics.c`, Files' and other apps' path literals (string change only).
6. **Runner, tests, suites.** `scripts/test/run.py` normal-boot milestones (`[supervisor] bootstrap: spawning /Programs/desktop`) and any path literal; `tests/host/test_normal_boot.py`; `tests/suites/*.json` predicates naming `/bin` or `/home` (f0-smoke, f2-*, f3-*); the host replay and gate harnesses (`tests/host/desktop/*.c`, `tests/host/desktop/*.py`). If a scripted desktop case changes, regenerate with `python3 tests/host/desktop/shell_menu.py --regenerate` (none expected: the menu does not show paths).
7. **Docs.** `docs/desktop.md`, `docs/build-and-run.md`, `apps/desktop/README.md` and other current documents naming these paths. Leave `dev_diary/` and the earlier `docs/directives/` files untouched (history).

## Method

`grep -rn` for `/bin`, `/home`, `"bin"`, `"home"`, `GAMES` across `src`, `apps`, `scripts`, `tests`, `docs`, `config`, `Makefile`, excluding `#!/usr/bin/env` shebangs and `/usr/bin` host tool paths, `legacy/`, `third_party/`, `build/`, `dev_diary/`, `docs/directives/`. Every hit is changed or justified in the report (for example host paths, DOS-side `C:\` strings that stay).

## Constraints

clang i686 `-Werror`; no QEMU, no `systemd-run`, no image builds, no commits, no diary entry, no files in the repository root or outside the worktree. Host suites green: `scripts/test/host_kernel_tests.sh`, `python3 -B -m unittest discover -s tests/host/desktop -t .` (after `make CAP= sdk` and `make CAP= desktop`), `tests/host/test_image_payloads.py`, `tests/host/test_normal_boot.py`, the runner tests (`tests/host/test_runner*.py` or as found). Use `nice -n 19` for builds and tests: a QEMU campaign runs on this machine.

## Deliverable

`build/codex/f2-35-report.md`: the list of changed files, the grep residue with justification, the verbatim summary lines of the host suites, and which QEMU suites the lead must re-run (expected: f0-smoke normal-boot, f2-desktop, f2-process, f3-vm vm-window).
