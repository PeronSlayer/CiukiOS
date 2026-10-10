# F0 regression on the integrated image (F1 drivers, F2 kernel, SDK payloads)

Date: 2026-10-11 (night of 2026-10-10). Image `d2daced5a81ee7133a2337db2f769ebcb8f11abee58a4fe72c7442ce3ec01799`,
clean build of commit `2b6e7cc` (`make build-full`: kernel 722,048 bytes with
FPU/SIMD audit, SDK 17.3 s, Lua 2.3 s, 50 hashed payloads read back from the
disk at T1). Profiles `qemu-t23`, `qemu-e500`, `qemu-min128` (TCG, icount).

What the image contains beyond F0: kernel services (f1-00), filesystem code
(f1-01), i8042 (f1-04), framebuffer device (f1-05), ATA PIO (f1-06), the F1
dispatch and probe registration (f1-03), native processes, ELF loader, memory
and threads (f2-02), `/bin/lua`, `/bin/hello`, `/bin/libc_smoke` and the Lua
test suite (f2-07). None of the F1 drivers is started at boot yet (directive
f1-08 pending); the F2 syscalls are dispatched but no F2 selector phase exists
yet (f2-09 pending). Signals (f2-04) were merged after this run.

| Suite | Result |
| --- | --- |
| `f0-smoke` | 2/2 PASS |
| `f0-core` | 56/56 PASS — 30 boots incl. warm restarts on the three profiles, eight non-destructive probes each, `video-fallback`, `safe-mode`; the `protection` probe now also verifies the GDT TLS descriptor (index 7, selector 0x3B) added by f2-02 |
| `f0-panic` | `panic` PASS; `uart-absent-qemu-t23` operator-confirmation subcase as in the F0 record |
| `f1-input` (dry run) | see below |

Run 00:32–00:45 UTC for the F0 suites. Host tests on the same tree: 54 runner
tests, kernel host suites (services, i8042 76,292 checks, ATA, SHA-256,
framebuffer 15,264 comparisons, ABI layout 687 checks, processes 94,456
checks), all PASS.

## f1-input dry run

71 cases: the suite first repeats the F0 regressions (59 PASS), then
`uart-absent-qemu-t23` reports its operator-confirmation FAIL as in `f0-panic`,
and the runner treats it as a failed prerequisite: the 11 F1 cases end
`not_run (prerequisite failed)`. The F1 probes were therefore not reached in
this run. Decision: operator-confirmation cases must not gate later cases
(directive f2-09 corrects the suite prerequisite rule); the F1 suites run for
evidence after directive f1-08 starts the drivers.

## Second image: `17524054e5950312…` (commit `a946a92`, with f2-09 and the f2: selector)

Built after the f1-03/f2-02/f2-04/f2-07/f1-07b/f2-09 merges (f1-08 not yet
included). `f0-smoke` 2/2 PASS. `f2-process`: the F0 regression prefix passed
(62 cases incl. `panic` on three profiles, `safe-mode`, `video-fallback`), but
the three `uart-absent-*` operator-confirmation cases still counted as failed
prerequisites, so the F1 regressions and the F2 cases (`elf-load`, `spawn-wait`,
`fd-table`) ended `not_run`: the rule introduced by f2-09 does not cover the
imported F0 regression cases. `f2-runtime` did not start: the runner's host
prerequisite (`tests/host`) failed in
`test_production_kernel_selector_names_and_phase_dispatch` because f1-08 had
been merged in the meantime and registers eight F1 probes where the test
expected the earlier table. Both defects go to directive f1-10. No F2 probe
has run in the guest yet.

## Third image: `0ba29b3e…` (commit `9a09809`, drivers started at boot by f1-08)

`f0-smoke` FAIL at once: `drivers_init` emits `probe=boot event=DATA
group=activation…` records before the boot probe's BEGIN, which the evidence
grammar forbids ("unknown event or missing BEGIN"). A manual 45 s boot with a
plain serial capture (`qemu-t23` settings, selector `f0:boot run=00000001`)
shows the drivers themselves come up: framebuffer `result=ready` (boot
console), input `backend=native result=ready`, `[ata0] identified=1`,
`[ata1] identified=0`, ATA `result=ready present=1`; then BEGIN, the usual
boot records, READY at tick 112, `END status=PASS`. Directive f1-11 moves the
activation evidence after BEGIN (ledger in the kernel, records from the boot
probe).
