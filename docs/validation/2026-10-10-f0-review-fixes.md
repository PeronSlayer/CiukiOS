# F0 review fixes (directive f0-01): QEMU evidence

Date: 2026-10-10. Scope: the nine findings of the Codex review of commit
`2003adb` (safe-mode automation, `core` selector, disk writer) and their
fixes, delivered by Codex under directive `docs/directives/f0-01-review-fixes.md`
and merged into `main` as `77f805e` (merge `da1bbe5`).

## Findings and fixes

| # | Finding | Fix | Where |
| --- | --- | --- | --- |
| 1 | `write_physical.sh` accepted any substring of the by-id path as confirmation | whole disk required, `lsblk` serial must be part of the stable name, typed answer must equal the serial | `866e5f1` (lead, urgent) |
| 2 | `core`/`all` continued after a failed probe | stop at the first failure, `NOT_RUN reason=prerequisite_failed after=<probe>` for the rest, then paging; runner and parser classify `not_run` | `probes.c`, `evidence.py`, `run.py` |
| 3 | 512-line screen history could lose `core` evidence | 1,536 lines, arithmetic in the comment (472 records × 3 lines + margin), static storage < 256 KiB | `console.c` |
| 4 | E500 safe-mode request built as `safe=1 platform=e500` | canonical reconstruction `f0:<probe> run=<id> [platform=e500] [safe=1]`, duplicate keys refused | `run.py` |
| 5 | configured video mode outranked safe mode | `mode_rank` skips the configured preference when `CBI_F_SAFE_MODE` is set; model test added | `video.inc`, `test_loader_model.py` |
| 6 | kernel parser accepted embedded NUL and unflagged suffixes | printable bytes only, ≤64 bytes, known names, `platform=e500` only with `CBI_F_INPUT_FORCED`, `safe=1` only with `CBI_F_SAFE_MODE`, both only with `CBI_F_SMBIOS_QEMU` | `probes.c` |
| 7 | timer predicates passed with zero ticks | relations `final_tick ≥ ready_tick + 10010` and `elapsed_pit_cycles = (final_tick − ready_tick) × 1193` (the kernel's exact formula) in every suite with the boot probe | `tests/suites/f0-*.json`, `evidence.py` |
| 8 | `loader_options` silently ignored | refusal restored | `run.py` |
| 9 | typed selector not echoed on screen | INT 10h teletype echo with backspace, UART unchanged | `menu.inc` |

Host tests after the fixes: `make test-host` 36 runner tests, 6 loader-model
tests, kernel library tests, audit fixture: all PASS.

## QEMU evidence

Profiles as in `tests/profiles/` (TCG, `-icount shift=1,sleep=on`; T23 512 MiB,
E500 256 MiB with `platform=e500`, min128). One QEMU at a time under the
runner's systemd scope.

### Worktree build (f0-01 alone), image `ba8ea695…f0ab2`

| Suite | Result |
| --- | --- |
| `f0-smoke` | 2/2 PASS |
| `f0-core` | 56/56 PASS — 30 boots (cold and warm restarts) on T23, E500 and 128 MiB profiles, the eight non-destructive probes per profile, `video-fallback`, and `safe-mode` (now automated through `safe=1`: `safe_mode=1`, 640×480) |
| `f0-panic` | `panic` PASS; `uart-absent-qemu-t23` reported as FAIL by design: the serial-disabled subcase requires operator-verified screen evidence (confirmed by the owner on 2026-10-09 for run `69573015`) |

### Merged `main` (f0-01 + f1-00 kernel services + f1-01 filesystem code), image `6444ad6b…fff05`, commit `e2f7023`

| Suite | Result |
| --- | --- |
| `f0-smoke` | 2/2 PASS |
| `f0-core` | 56/56 PASS (same matrix as above) |
| `f0-panic` | `panic` PASS; `uart-absent-qemu-t23` operator-confirmation subcase as above |

Run 22:35–22:46 UTC, kernel `f1fad06b…4241`. The f1-00 kernel services and the
f1-01 filesystem code are linked or present in the tree but dormant: no
driver uses them yet, so the F0 behaviour is unchanged.

## Not verified on QEMU

The `NOT_RUN` path of `core`/`all` needs a failing probe; no build variant
was made to force one (test architecture: one canonical image). It is
covered by the parser and runner host tests and by Codex's scratch checks
of all 18 failure positions. It will be exercised on hardware if a probe
fails there.
