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

## Fourth image: `acd8af37…` (commit `b6c1562`, f1-11: activation records after BEGIN)

`f0-smoke` 2/2 PASS, `f0-core` 56/56 PASS, `f0-panic` `panic` PASS plus the
operator-confirmation subcase; run 01:42–01:55 UTC. The boot probe now emits
the activation records after BEGIN (`[init]` klog lines, then
`group=activation_flag`, `group=activation device=framebuffer|input|ata
result=ready`). This is the first image on which the F0 gate passes with the
F1 drivers started at boot.

## Fifth image: `0e4261b5…` (commit `5a654a7`, f1-09 storage, f1-10, f2-05)

Every F0 suite fails at once: `storage_init` emits `probe=boot event=DATA
group=storage …` before the boot probe's BEGIN (same class as the f1-08
defect). The drivers and the mount itself come up (`[init]` lines show
framebuffer, input and ATA ready; the storage record reports the read gate).
Directive f1-13 routes storage evidence through the activation ledger and adds
a static guard against `rec_emit` outside probe files, plus a runtime counter.

## Sixth image: `b0cb346a…` (commit `5e7eaee`, 53 payloads incl. the desktop)

F1 and F2 suites, 03:05–04:46 UTC. F0 regressions pass in every suite.
First F1 probes passing in the guest: `registry`, `safe` on the native
profile (`safe-fw-cfg-qemu-t23`, `safe-boot-cfg-qemu-t23`), `ata`,
`partition`, `ata-fault` (`ata-fault-blkdebug` declared `not_run`).
Failures: `input-fault` (fixture allocation above the 2,040-byte kmalloc
limit — fixed by f1-14), `safe-fw-cfg-qemu-e500` (firmware input backend
init `error=-5`, directive f1-15), `fat12-read` (superfloppy fixture disk not
mounted, directive f1-16). The F2 cases were not reached because every F2
suite repeats the F1 regressions and stops at `input-fault`.

## Seventh image: `409fbf47…` (commit `4990941`, f1-14 and f2-03 merged)

No suite ran: the runner's host-prerequisite step failed on two stale host
tests (`test_records_and_manifest_include_every_payload` needs the
`build-clock.json` the f2-03 patch introduced; `test_kernel_map_f2probes_within_rodata`
expects the F2 probe order without `fd-table`). Fix in progress; the
summaries left under `build/test-runs/` for this image are the previous
batch's and carry no evidence for it.

## Eighth image: `a2c89862…` (commit `09b4ecb`, f1-14 and f2-03 merged, host tests green)

`f0-smoke` 2/2 PASS. `f1-input`: `registry` PASS, `input-fault` PASS (after
f1-14), `input-qemu-t23` PASS — the contract stimulus through QMP (100 `a`
make/break cycles, 100 relative moves, 10 button cycles) reached the native
driver with the expected counts; `input-qemu-e500` FAIL (firmware backend,
fixed by f1-15 after this run), so `framebuffer` and the firmware subcases
stayed `not_run`. The F2 suites of this batch were stopped by the lead once
their F1 prefix hit the same e500 case; no F2 case ran.

## Ninth image: `f76ab59e…` (commit `5ccc80fd`, f1-15 and f1-16 merged)

Suite batch: `f0-smoke` 2/2; `f1-input` and `f1-safe` stopped in their F0
prefix at `preempt` on `qemu-e500` (`switches=0 … dispatch_a=0`): the firmware
queue thread introduced by f1-08 and now active after f1-15 polled the BIOS VM
every millisecond at `P_DEVICE` and starved ring-3 tasks (directive f1-18,
merged). Manual single-probe boots of the F2 probes on the `qemu-t23`
configuration (diagnostic signal, not suite evidence): `elf-load` PASS (31
records), `spawn-wait` PASS (157), `mmap` PASS, `threads-wait` PASS,
`crash-isolation` with the stand-in server PASS (100 cycles, ledger zero);
`signals-fault` FAIL only because `syscall-interruption` was marked not_run
pending f2-03; `fd-table` FAIL on three unwired subcases (`uname`, post-commit
fault, durable checker); `libc-smoke` FAIL: the SDK newlib program
`/bin/libc_smoke` ran in the guest for the first time (11,240 checks, 3
failures, exit 1, stdout `CiukiOS libc smoke: 4294967297 1.25`). Directive
f2-10 completes these probes.

## Tenth image: `aee7f539…` (commit `408d056`, f1-17 and f1-18 merged)

Suite batch on `qemu-t23`, `qemu-e500` and `qemu-min128`: `f0-smoke` 2/2;
`f0-core` 56/56 (the `preempt` starvation of the ninth image is gone after
f1-18); `f1-input`: `registry`, `input-fault`, `input-qemu-t23` PASS,
`input-qemu-e500` FAIL (no events after ARM through the firmware backend:
retained virtual-PIC IRQs, fixed by f1-19 after this run), so `framebuffer`,
`framebuffer-no-lfb`, `firmware_overrun` and `disallowed_io` stayed
`not_run`; `f1-safe`: every QEMU case PASS on both laptop profiles plus
`safe-no-storage` and `safe-no-lfb` (the two `safe-menu-*` cases need
physical menu evidence); `f1-storage`: `ata`, `partition`, `ata-fault` and
`ata-fault-blkdebug` PASS on every profile; `f1-fat32`: `fat12-read`,
`fat16-read`, `fat32-read` PASS, `cache` FAIL with `END error=-16` after all
its subcases passed, which left the 13 later cases `not_run`. The three
`uart-absent-*` operator-confirmation cases count as failures in the
summaries and gate nothing. The F2 suites were stopped by the lead at their
F1 prefix.

Single diagnostic boots of the write probes on the same image (`qemu-t23`,
not suite evidence): `f1:fat-write` (workload and digests `result=0`, then
`flush_result=-16`), `f1:cache` and `f1:bootlog` (`qualification … writes=1
flush_result=0`) all end `FAIL error=-16`. The error is `storage_sync()` →
`storage_shutdown()` → `vfs_detach(C:)` → `px_volume_busy()`: since f2-03
`files_bootstrap` pins the supervisor's cwd on the root node at every boot,
and the F2 busy hook treats that pin as an open description. The host
storage test does not link the namespace, so it never saw it. Directive
f2-11 fixes the shutdown rule; the `fd-table` durable checker of
`f2-process` calls `storage_sync()` the same way.

## Eleventh image: `3d79e6a7…` (commit `5e6b2a4`, f2-10, f1-19 and f1-20 merged)

Five QEMU profiles (`qemu-t23`, `qemu-e500`, `qemu-min128`,
`qemu-desktop-1998`, `qemu-desktop-2002`). `f1-input` 102 pass: the F0
prefix on every profile, `registry`, `input-fault`, `input-qemu-t23` and
`input-qemu-e500` PASS (the E500 firmware backend delivers the QMP stimulus
after f1-19); `framebuffer` FAIL with `error=-12` before any drawing: the
probe's 2,247-byte fixture exceeds the kernel heap's 2 KiB class (directive
f1-21), which left `framebuffer-no-lfb`, `firmware_overrun`, `disallowed_io`
and the eight `qemu-desktop-*` cases `not_run`. `f1-safe` 106 pass: every
QEMU case on all five profiles, including `safe-fw-cfg` and `safe-boot-cfg`
on the two desktop profiles; only the two physical `safe-menu-*` cases stay
`not_run`. The three `uart-absent-*` operator-confirmation cases count as
failures and gate nothing. Host tests on this tree: 102 OK after the SDK
rebuild that the f2-10 change to `sdk/tests/libc_smoke.c` required.

## Twelfth image: `2a457bb8…` (commit `92da010`, f2-11, f1-21, f2-12 and the first f2-14 delivery merged)

`f1-fat32` on `qemu-t23`, `qemu-e500` and `qemu-min128`: 106 pass. With
the storage shutdown fixed by f2-11 the write cases run for the first time
in suite context: `cache`, `cache-unsupported-flush`, `cache-delayed-error`,
`cache-flush-error`, `fat-write`, `bootlog` and `bootlog-read-only` PASS;
`mount-bad-bpb` FAIL (`corrupt_fixtures status=not_run reason=absent`: the
runner attaches no fixture for the `injection` declarations, directive
f1-22), leaving the six later `mount-*` cases `not_run`. Single boots on
`qemu-t23` of the same image: `f2:signals-fault` now passes `fault-repair`
(`tcg_fallback=1`) but still fails `nanosleep-eintr` (`elapsed_ms=40
result=0`; second f2-14 delivery pending), and the runner case
`crash-isolation-normal-qemu-t23` with `server=desktop` fails at the launch
handshake without a diagnostic record (directive f2-16). Earlier image
`a9550f04…` (commit `3619267`): `fat-write`, `cache`, `bootlog`,
`framebuffer`, `elf-load`, `spawn-wait`, `mmap`, `threads-wait`,
`libc-smoke`, `crash-isolation` (stand-in) and `fd-table` (kernel verdict,
with the FAT16 fixture) PASS in single boots; `app-gate` reports
`missing_probe` (no probe was registered, directive f2-15).

## Thirteenth image: `dae098ef…` (commit `b09a614`, f2-13 and f2-15 merged) and `a4806d0c…` (commit `2326aee`, f2-16 diagnostics)

Runner cases on `qemu-t23` without prefix (diagnostic signal for the
aligned F2 suites, image `dae098ef…`): `elf-load` PASS, `mmap` PASS;
`spawn-wait` and `signals-fault` ended with the kernel `END status=PASS`
but failed two predicates written against older captures (`fault`
`address ge 1` while the null-pointer victims fault at address 0; the
`fault-repair` indices shifted by one after the f2-14 TCG `#AC` fallback,
index 6 = alignment step, index 7 = `#MF`). With the predicates corrected
(commit `13f96e2`) both cases PASS through the runner on the same image;
`fd-table`, `threads-wait` and `libc-smoke` were not reached in those
partial runs. `app-gate` double-faults in the probe task while building the
metadata records (`PANIC vector=8 esp=f0007000 cr2=f0006ffc`: kernel stack
overflow, directive f2-17). On image `a4806d0c…` the desktop-mode
`crash-isolation` run reports `case=launch … survivor=0 control=00000000
stage=0 ticks=360 reason=setup:survivor:9` and the desktop exits with
status 256: the gate setup fails at the survivor spawn with `EBADF`
(directive f2-16, second round). Host tests on commit `cd32ee6`: 131 OK
after the alignment fixture and the SDK smoke test were updated for the
passing `signals-fault` capture and the three clock reports.
On image `a4806d0c…` the remaining `qemu-t23` cases of `f2-process` and
`f2-runtime` pass through the runner without prefix: `fd-table` (with its
FAT16 fixture, ARM, durable export and host checker), `threads-wait` and
`libc-smoke`; together with `elf-load`, `spawn-wait`, `mmap` and
`signals-fault` all seven cases of the two suites pass on `qemu-t23`.
