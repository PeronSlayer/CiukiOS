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
The same seven cases also pass through the runner on `qemu-e500` and
`qemu-min128` (14 of 14), so `f2-process` and `f2-runtime` are green on the
three laptop/minimum profiles for image `a4806d0c…`.
F1 cases of `f1-input` through the runner without prefix on image
`a4806d0c…`: `registry`, `input-fault`, `input-qemu-t23`, `input-qemu-e500`,
`framebuffer` (after f1-21) and `framebuffer-no-lfb` PASS; `firmware_overrun`
ends `NOT_RUN` by the suite's own declaration (the BIOS VM self-test has no
record emitter) and the runner marks the later `qemu-desktop-*` cases
prerequisite-failed (directive f1-23).

## Fourteenth image: `3933d34f…` (commit `5205433`, f2-16 both rounds, f2-17 merged)

Runner case `crash-isolation-normal-qemu-t23` with `server=desktop`: the
launch handshake now completes (`case=launch … survivor=4 control=20301000
stage=2 ticks=1059 reason=ok`, identity record with distinct PIDs and
process groups) and 62 victim cycles run before the runner stops the case
with `application stream identity limit exceeded`: every victim's call-3
report is also framed as an application stream (`group=app … stream=report`)
and the evidence parser caps distinct (pid, tid, stream) identities at 64
(f2-16 third round). On image `d3db42dd…` (commit `0147f99`, f2-17) the
`app-gate` probe no longer double-faults: both Lua programs run in the
guest; the supplement passes in 128 s, the upstream run reaches `files.lua`
after 68.7 s and fails its first assertion (`os.getenv"PATH"`, directive
f2-18).

## Fifteenth image: `622e1632…` (commit `3d969fb`, f1-22) and `abaa1064…` (commit `deb43ca`, f1-23)

Runner cases without prefix on `qemu-t23`, image `622e1632…`: the six
corruption cases `mount-bad-bpb`, `mount-dirty`, `mount-error-flag`,
`mount-fat-divergence`, `mount-chain-corruption` and `mount-torn-sector`
PASS with the deterministic FAT32 fixtures (mount refused with `-22`, or
read-only with the expected reason and `EROFS` refusal, zero writes);
`mount-crash-reboot` FAIL: boot 1 placed the marker but the write
suspension was armed from launch, so the guest never reached `ARM
action=crash_cut` (f1-22 follow-up). Image `abaa1064…`: `firmware_overrun`
and `disallowed_io` PASS on all five profiles (records from the BIOS VM
self-test: `timeout_result=-110 timeouts=1 disabled=1 later_result=-5`,
`policy_result=0 denied_result=-1 disallowed=1`, PIC/PIT unchanged), and
the eight `qemu-desktop-1998/2002` cases of `f1-input` (`registry`,
`input-fault`, `input`, `framebuffer`) PASS. Host tests: 153 OK.
On image `abaa1064…` the six stand-in `crash-isolation` cases of
`f2-desktop` (`no-lfb` and `safe` on `qemu-t23`, `qemu-e500`,
`qemu-min128`) PASS through the runner.

## Sixteenth image: `da10a7ad…` (commit `2d6d07d`, f2-16 third round)

`crash-isolation` with `server=desktop` PASS through the runner on all five
profiles (`qemu-t23`, `qemu-e500`, `qemu-min128`, `qemu-desktop-1998`,
`qemu-desktop-2002`): launch handshake `reason=ok`, 100 victim cycles over
the five fault kinds with `desktop_restarts=0`, post-fault interaction
consumed (`input_events` 0 → 8, `presents` 924 → 931, pixel digest changed,
keys=2 motion=3 buttons=2) and the runner's screen observation (portrait
region and cursor change) passed. Host tests: 155 OK.

## Seventeenth image: `33a68c43…` (commit `62bd77f`, f2-18 and the f1-22 follow-up merged)

Runner case `app-gate-qemu-t23`: the unmodified upstream Lua suite with
`_U=true` exits zero in the guest (`case=lua-basic exit=0 final_ok=1
assertion_failures=0`, 72.9 s) and the supplement passes (127.8 s, four
cases); the desktop stays alive and advances; every process-level ledger
is balanced; the verdict fails only on unattributed storage-cache and
namespace growth (`pages_delta=32 kernel_bytes_delta=1024`, cache nodes 59
→ 60, `cache_accounted=0`), directive f2-19. Runner case
`mount-crash-reboot`: the gate arms after ARM and cuts at the declared
index 2; the exported volume shows an orphaned long-name part of
`F109CUT.BIN` and the dirty bit, which the runner reports as an
unclassified interrupted-state outcome and boot 2 is not attempted
(directive f1-24). Host tests: 160 OK.
`f0-core` on image `33a68c43…`: 92 of 92 PASS on the five profiles
(`qemu-t23`, `qemu-e500`, `qemu-min128`, `qemu-desktop-1998`,
`qemu-desktop-2002`).

## Eighteenth image: `9ae41b27…` (commit `6084e21`, f1-24)

Runner case `mount-crash-reboot` PASS on `qemu-t23`: boot 1 cut at the
declared index 2 with the orphaned long-name part classified as the
declared interrupted outcome; boot 2 on the same overlay reports
`case=crash_reboot reasons=5 lost=0 scan_corrupt=0`, `crash_refusal
write_refusal=-30 writes=0`, `coverage cut_selected=1 cut_reboot=1`. Host
tests: 170 OK.

## Nineteenth image: `593b18b5…` (commit `e85e0be`, f2-19)

Runner case `app-gate-qemu-t23` PASS: both Lua runs exit zero and the
resource ledger attributes the growth (`identity_bytes_delta=1024`,
`identity_nodes_delta=1`, heap pool pages, `pages_remainder=0`,
`kernel_bytes_remainder=0`, `cache_bounded=1`). First combined `f2-all`
batch (one runner invocation, 191 cases): 142 PASS through `f0-smoke`,
`f0-core`, `f0-panic`, `f0-runner`, `f1-input`, `f1-storage`, `f1-safe` and
`f1-fat32` up to `bootlog`, including every case that had failed earlier in
the day; 4 FAIL = the three `uart-absent-*` operator-confirmation cases
and `bootlog-read-only` (its `driver-boundary read-only` fixture
declaration is not implemented by the runner, directive f1-25), which left
the 45 F2 cases `not_run` by prerequisite in that invocation.

## Twentieth image: `b9c052cd…` (commit `ac20b25`, f1-25) — final `f2-all` batch

SHA-256 `b9c052cd75a8b8e51680d35de9834894f0c7372d37f3c0eca269e319ae726c41`,
build manifest clean at `ac20b25`, host tests 178 OK. One runner
invocation `f2-all` (14:10–14:59, `-icount shift=1`), 191 cases over
`qemu-t23`, `qemu-e500`, `qemu-min128`, `qemu-desktop-1998` and
`qemu-desktop-2002`: **186 PASS, 0 FAIL apart from the three
`uart-absent-*` operator-confirmation cases (screen captured, external
confirmation required by contract), 2 `not_run` = the two physical
`safe-menu-*` cases.** Every suite passed in order: `f0-smoke`, `f0-core`,
`f0-panic`, `f0-runner`, `f1-input`, `f1-storage`, `f1-fat32` (including
`bootlog-read-only` through `safe=1`), `f1-safe`, `f2-process`,
`f2-runtime`, `f2-desktop` (five desktop and six stand-in cases) and
`f2-app` (three profiles). This image is the single i686 image handed to
the hardware qualification.

First hardware boot of this image on the ThinkPad T23 (Transcend
TS64GMSA230S written with `write_physical.sh`, SHA verified on write):
the kernel boots from the disk, reports 523 260 KiB usable RAM, 16
registry reservations and 15 PCI functions, framebuffer ready, ATA disk 0
identified, C: mounted read/write with the boot log on disk
(`disk_log=available`); the ring-3 desktop appears with the portrait and
the tagline. On one boot the PS/2 input initialisation reported
`input result=failed error=-5` and the desktop did not start; the next
boot succeeded. Serial capture of these boots was not available, so the
T4 records are still to be collected.

## Twenty-first image: `9218d595…` (commit `6da9b9c`, f1-26 dirty-volume recovery)

`f2-all` in one invocation (15:37–16:27): 186 PASS, the three
`uart-absent-*` operator-confirmation cases, two physical `safe-menu-*`
`not_run`; `mount-dirty` and `mount-crash-reboot` now pass with the
recovery predicates. On the ThinkPad T23 (serial capture `a1b2c3d6`): the
boot after an unclean power-off mounts C: read/write with
`reason=dirty_recovered writes=4` and the boot log on disk; `f0:all`
passes every F0 probe up to the deliberate `panic`; `f1:all` passes
`registry` and stops at `input` (`case=setup backend=native error=-5`,
directive f1-27).

## Hardware: first unattended sweep on the ThinkPad T23 (image `cb33aec3…`, commit `f7286ca`, capture `44444444`)

`BOOT.CFG` with `probe=all:sweep run=44444444`; 33 boots driven by the
cursor and the kernel's self-reboot, two power cycles by the operator
(after the deliberate F0 `panic` and after a double fault). Results:
F0 `boot`, `bootinfo`, `allocator`, `protection`, `isolation`, `preempt`,
`localfault`, `syslife`, `fpu` PASS, `panic` `not_run
reason=reset_before_completion` (intentional halt, recovered by power
cycle). F1 `registry`, `input-fault`, `framebuffer`, `ata`, `ata-fault`,
`partition`, `cache`, `fat-write` (two boots), `mount-crash` (boot 2),
`bootlog` (two boots) PASS; `input` FAIL (`iface_kbd` `0xAB` → `0xFA`,
directive f1-29, merged after this run); `fat-read` FAIL `error=-5` with
no fixture disk present (directive f1-30); `safe` FAIL (the cursor cannot
be persisted in safe mode, documented limit); `mount-crash` boot 1
`not_run reason=reset_before_completion` (the cut needs the runner). F2
`elf-load`, `spawn-wait`, `mmap`, `threads-wait`, `libc-smoke` PASS;
`crash-isolation` with the real desktop: 100 victim cycles PASS, desktop
and survivor alive, then the post-fault interaction FAIL with
`input_events=0` (no input driver on this image); `fd-table` double fault
after `inherit-cloexec` (`esp=f0006ff4`, directive f2-20); `signals-fault`
and `app-gate` not reached before the halt. The i8042 step records of
f1-27 named the failing keyboard step on the first boot. Image
`dfa78c3e…` (commit `f86f350`, f1-29): QEMU input cases PASS and the
runner's `sweep-smoke` case PASS (ten boots under `-icount`, final panic
recovered).

## Hardware: second unattended sweep on the ThinkPad T23 (image `90477716…`, commit `abcd677`, capture `66666666`)

35 boots, one operator power cycle after the F0 `panic`, `SWEEP_END
passed=27 failed=6 not_run=2`. Changes against the first sweep: the
native input driver initialises (`input result=ready`, all 15 setup
steps `result=0`, `iface_kbd` reply `0xFA` accepted); `fd-table` no
longer double-faults (`case=stack task=probes size=16384
high_water=8356`) and matches every operation except the fixture-backed
`exdev`/`readonly` pair (`missing_second_volume`); `signals-fault` now
runs to its verdict with the real `#AC` (`raw_vector=17`) but
`fault-repair` counts `entries=5 returns=4`; `app-gate` PASS on hardware
(`lua-basic exit=0 final_ok=1`, `lua-supplement exit=0 final_ok=1`);
`crash-isolation` with the real desktop: 100 cycles PASS, interaction
`input_events=0` (no operator). Still failing for infrastructure
reasons: `input` (`stimulus_or_lease`, no operator), `fat-read` (the
sweep's cursor write counts as a volume write, so the `fixtures_absent`
path is never reached), `safe` (cursor not persisted in safe mode).
Everything else as in the first sweep. The import of this capture with
the runner was refused (`build_id` field, then the per-boot sequence
check): directive f1-33.

## Twenty-second image: `5fe9446d…` (commit `96440d6`, f1-31, f1-32, f2-21, f2-22 merged)

`f2-all` in one invocation (21:10–21:57): 186 PASS, the three
`uart-absent-*` operator-confirmation cases, two physical `safe-menu-*`
`not_run`; host tests 211 OK. Targeted QEMU checks before the batch:
`input` (T23/E500), `fat16-read`, `fat-write`, `bootlog`,
`bootlog-read-only`, `fd-table`, `signals-fault` (T23/E500) and
`crash-isolation` with the real desktop all PASS. The hardware image for
the third T23 sweep is the same tree with `BOOT.CFG`
`probe=all:sweep run=77777777` (`build/f0/ciukios-hw.img`,
SHA-256 `6ed146ab…`).
Physical import of capture `66666666` with the f1-33 importer
(`run.py f2-all --physical-capture … --image-sha256 90477716…`): 35 boots,
`sweep_complete=True`, physical summary 56 PASS, 1 FAIL (`video-fallback`:
its `text eq 1` predicate describes the QEMU `-vga none` boot; on a machine
with an LFB the case must map to `not_run`, hardware-tier follow-up),
132 `not_run` by the prerequisite rule and the hardware-tier reasons
(`fixtures_absent`, `second_volume_absent`, operator cases). The real
captures serve as local test fixtures (`tests/host/fixtures/physical/`,
log files ignored by git, tests skip without them) because physical logs
are never published.

## Hardware: third unattended sweep on the ThinkPad T23 (image `6ed146ab…`, commit `96440d6`, capture `77777777`, operator at the prompts)

`BOOT.CFG` `safe=0 serial=1` and `probe=all:sweep run=77777777`
(`build/f0/ciukios-hw.img`, the `5fe9446d…` tree with a different
`BOOT.CFG`); 35 boots, one operator power cycle after the F0 `panic`,
`SWEEP_END passed=29 failed=4 not_run=2`. Changes against the second
sweep: `fat-read` PASS with the `fixtures_absent` `not_run` subcase (the
cursor write is no longer counted, f1-32); `fd-table` PASS with
`exdev`/`readonly` `not_run reason=second_volume_absent` (f2-21);
`signals-fault` `fault-repair` `entries=7 returns=7` with the real `#AC`
(f2-22 confirmed on hardware); `app-gate` PASS; the operator prompts of
f1-31 appeared and were answered. Still failing:

- `input`: the operator produced `characters=113 key_transitions=200
  button_transitions=74 events=3150 x=3667 y=-4760`, queue `overflow=0
  resync=0 duplicates=0 repeats=0 errors=0 stuck_keys=0`, lease retained;
  FAIL `stimulus_or_lease` because the verdict requires the exact QEMU
  totals (100 `a`, 20 button transitions, `x=200 y=-100`): directive f1-34.
- `crash-isolation`: 100 victim cycles PASS (`desktop_restarts=0`); the
  interaction counted `keys=8 motion=61 buttons=0`, then `case=step
  command=6 … live=0 reason=server_exit:256`: the desktop exited with
  status 1 under the operator's event stream (the gate loop fails a
  report when a client still has pending transmissions); the driver saw
  74 button transitions in the same sweep: directive f1-34.
- `signals-fault`: the only non-conforming subcase is `channel-eintr`
  (`entries=0 returns=0 result=0 releases=0 handler_completion=0`,
  survivor progress `382→3403`): the injection predicate never held
  within the 2 s deadline and the probe stopped the payload: directive
  f2-23.
- `safe`: as in the earlier sweeps the step runs with `safe_mode=0`
  (the cursor is not persisted in safe mode); `optional_activations=2`,
  `subcase=required owner=i8042 errors=1`, FAIL `safe_contract`.

`panic` and the first `mount-crash` boot are `not_run
reason=reset_before_completion` as designed. The import of this capture
was refused with `sweep boot lacks BOOT.CFG provenance`: the boot after
step 18 (`cache`) carries `L:SELECT_SOURCE=fg` after a byte loss at the
self-reboot (34 intact provenance lines out of 35 boots). The
`video-fallback` FAIL of the `66666666` import is a QEMU-only case
(`device_exceptions` `vga: none`). Both import rules are directive
f1-35; the results above are read from the capture until the import
lands.

## Twenty-third image: `a71327b7…` (commit `798a92d`, f2-23, f1-35, f1-36, f1-34 and the lead's importer glue merged)

Changes under test: `channel-eintr` injection accepting the timer-expired
`T_READY` receive with `part=inject`/`part=wait` records (f2-23); the sweep
`safe` step as `NOT_RUN reason=safe_mode_not_selected` (f1-36); the
operator criteria of the `input` probe and the desktop gate loop that
coalesces reports instead of exiting on pending transmissions, with
button edges counted and typematic repeats tolerated (f1-34 and the lead
amendment); the importer rules (f1-35 plus the lead's glue: damaged
provenance with sweep continuity, `device_exceptions`, QEMU-only
` safe=1` selector cases, `timing_domain` literal, operator criteria).
Host tests 219 OK. Targeted QEMU checks through a scratch suite
(prerequisites empty, deleted after the run): `input-qemu-t23`,
`input-qemu-e500`, `input-qemu-desktop-1998`,
`crash-isolation-normal-qemu-t23`, `crash-isolation-normal-qemu-e500`,
`signals-fault-qemu-t23`, `signals-fault-qemu-e500`,
`signals-fault-qemu-min128`, `safe-fw-cfg-qemu-t23`,
`safe-boot-cfg-qemu-t23`, `video-fallback`: 11 PASS. The physical import
of capture `77777777` with the merged importer (`--image-sha256
6ed146ab…`): 98 PASS, 1 FAIL (`input-qemu-t23`, the historical operator
run judged by the exact totals of that image), 90 `not_run` (74 by the
`input` prerequisite, 6 `device_exception_not_applicable:vga=none`,
5 `selector_flag_not_applicable:safe=1`, 3 `serial=none`, 1 host-block
fault variant, 1 `disk_cache=writethrough`); `66666666` imports with the
same rules. The hardware image for the fourth T23 sweep is the same tree
with `BOOT.CFG` `safe=0 serial=1` and `probe=all:sweep run=88888888`
(`build/f0/ciukios-hw.img`, SHA-256 `ad63279d…`). The `f2-all` batch on
`a71327b7…` is recorded below when it completes.

## Twenty-fourth image: `7ab10f64…` (commit `a9c0b31`, f3-03 merged)

Built after the f3-03 merge (XMS/EMS/UMB services, DOS VM boot fixes,
F3 platform override; diary 2026-10-11-12). `f0-smoke` 2/2. The `f3-vm`
and `f2-all` runs were first refused by the runner's host prerequisite:
the stack allocator host test still asserted 16 KiB kernel stacks after
the 32 KiB change of `31ed0c5` (fixed in `a9c0b31`). The rerun of `f3-vm`
passed its F0 prefix up to `isolation` and failed `preempt`
(`switches=0 timer_switches=0 dispatch_a=0 dispatch_b=0` over 120020 ticks):
the AC'97 refill thread started by `31ed0c5` polled every millisecond at
`P_DEVICE` (`kwait_wait_until` with a 1 ms deadline) and starved the ring-3
tasks, the f1-18 pattern. No image with the audio worker running had been
put through the F0 probes before.

## Twenty-fifth image: `0aa939e9…` (audio worker blocks between services; VM records named by their probe)

The refill thread now blocks with a 1/4/50 ms safety deadline (burst,
running, idle) and VM admission/stop/guest records carry the owning probe
(`vm-boot`, `vm-memory`, `vm-devices`) through `dosvm_config.probe`. The
`f3-vm`, `f0-smoke` and `f2-all` results on this image are recorded below.
`f0-smoke` 2/2. The first `f3-vm` run was refused by the host prerequisite:
`test_ntfs_ro.test_fixture_generation_is_deterministic` compared a repeat
generation against the session's fixtures and every image hash differed;
run alone immediately afterwards the same test passed in 15 s, so the
generation is not deterministic under concurrent load (an f4-03 follow-up:
pin every tool-written field, not only the serial and times). Suites are
rerun on a quiet machine.

## Twenty-sixth image: `fcd96d6e…` (commit `9f17715`, fat-read digest cap)

Built after the fat-read cap (files above 8 MiB are recorded as `case=large`
in the volume walk; `fat12-read` had exceeded its host deadline on the
128 MiB DOS boot image) on a quiet machine, with the deterministic NTFS
fixtures and the 32 KiB stack assertion in place. Results, each suite
preceded by the runner's host prerequisite (303 Python tests OK):

- `f3-vm` 195 PASS: the F0–F2 prefix, then `vm-boot-qemu-t23/e500/min128`
  and `vm-memory-qemu-t23/e500/min128` (second VM admitted on every
  profile, conformance `xms_calls=007fffff ems_calls=3fffffff`). The three
  `uart-absent-*` cases remain operator cases (screen evidence).
- `f0-smoke` 2/2.
- `f2-all` 189 PASS, the same three operator cases excepted.

This is the first integrated image on which DOS VMs boot and the F3 rows
f3-00 and f3-03 hold QEMU evidence. The hardware image for the fifth T23
sweep is built from the same tree with `BOOT.CFG` `safe=0 serial=1` and
`probe=all:sweep run=99999999` (`build/f0/ciukios-hw.img`, SHA-256
`e4db63968b932fd0b5e6b1786fb609fdd05d8d495ad514b6f42a796deeec732b`). Note
for the next hardware image: `build_image.py` writes `build-manifest.json`
beside its output, so an `--out` inside `build/f0/` replaces the canonical
image's manifest; rebuild the canonical image afterwards (or write the
hardware image elsewhere).


## Twenty-seventh image: `e896ed38…` (commit `a90fd53`, f3-01, f3-02 and f4-04 merged)

Campaign 9 ran `f3-vm` first: the F0–F2 prefix passed (189 PASS, the three
`uart-absent-*` operator cases excepted), then `vm-boot-qemu-t23` failed on
`devices_before eq 3`. The probe itself reports PASS (`case=baselines
… devices_before=4 devices_after=4 restored=1`): since f3-02 every VM counts
four authoritative devices (PIC, PIT, text/input service, VGA adapter) in
`dosvm_snapshot`, and the three `vm-boot` cases still pinned three. The pin
is corrected (`daff47e`); the campaign chain stopped at that failure, so
`f0-smoke`, `f2-all` and `f4-media` did not run on this image. No defect
in the image is known.

## Twenty-eighth image: `4595cd98…` (commit `daff47e`, desktop batch f2-31, f2-32, f2-33 merged)

Campaign 10 ran only the suites the two merged batches can influence,
through scratch suites without the F0–F2 prefix (`zz-f3`: the 14 F3 cases;
`zz-desk`: the 16 `f2-desktop` cases with the three new app cases' pixel
digests left unchecked for the first run), then `f2-process`, `f2-app`,
`f4-media`, `f0-smoke`:

- `zz-f3`: `vm-boot-qemu-t23/e500/min128` PASS (four devices per VM);
  `vm-memory-qemu-t23` FAIL. Two findings: the guest reported `VMMEMORY
  FAIL A7` (the 8042 output-port read-back: since f3-01 a controller reply
  raises IRQ1 like real hardware, and the CIUKIDOS keyboard ISR consumed the
  D0h reply before the payload polled the status port; the payload now runs
  the D1h/D0h sequence with interrupts off), and the probe's screen dump of
  the failed VM carried `\` and `>` in a record value, which the runner's
  grammar refuses (`invalid key=value grammar`), hiding the real failure;
  the dump now maps every glyph outside `[A-Za-z0-9_:.,/+-]` to `.`. The
  remaining ten F3 cases did not run.
- `zz-desk`: 13 PASS (`crash-isolation-*`, `desktop-shell-qemu-t23/e500`);
  `files-qemu-t23` FAIL: the desktop recognised F11 (`files-write-gate
  observed=0`) but no `files-checkpoint` record followed, the interaction
  record ended with `keys=0 motion=41 buttons=14` at the probe's 40 s input
  deadline while the runner had delivered 138 of the 176 stimulus batches
  (QMP `input-send-event` costs about 0.29 s per batch on this host, pauses
  included). The probe's app-case deadline is now 180 s; the three app
  scripts are being replayed on the host against the real desktop and app
  code before the next run. The runner now keeps `screen-fail.ppm` for a
  failed desktop or DOS-window case.
- `f2-process`, `f2-app`, `f4-media`: REFUSED by the host prerequisite
  (`test_dosvm.test_suite_needs_every_cycle_and_cleanup` still modelled three
  devices per VM; corrected). `f0-smoke` 2/2.

Evidence on this image therefore covers `vm-boot` on three profiles, the
thirteen existing desktop cases and the boot smoke; everything else waits
for the next image.

## Twenty-ninth image: `9fbfab4d…` (commit `edc8137`, version 0.9.0 pre-alpha, f3-05 and f3-04 merged)

Campaign 11 ran the F3 cases beyond `vm-boot` as a scratch suite (`zz-f3b`,
17 cases), then `f2-process` and `f2-app` with their F0–F2 prerequisites.
Host prerequisite: 336 tests OK.

- `zz-f3b`: `vm-memory`, `vm-devices` and `vm-video` PASS on qemu-t23,
  qemu-e500 and qemu-min128 (nine cases; the VMMEMORY 8042 sequence under
  CLI resolved the A7 failure of campaign 10). `vm-window-qemu-desktop-2002`
  FAIL, "DOS window region mismatch": the keystrokes typed through QMP and
  the desktop did not reach stage 1 within the probe's 30 s, the probe fell
  back to kernel typing (`typed=kernel`), VMVIDEO then advanced to stage 4
  before the frame was sampled (`mode=19 320x240`), and the retained
  `screen-fail.ppm` shows the kernel screen console drawn over the desktop
  launched by the probe, with the desktop background never painted. The
  remaining cases (`vm-window-qemu-desktop-1998`, `vm-audio-state` ×3,
  `dpmi` ×3) did not run. Diagnosis assigned; see the next image.
- `f2-process`: 161 PASS including the prerequisites; the three
  `uart-absent-*` operator cases excepted.
- `f2-app`: 58 PASS, then `safe-mode` (prerequisite `f1-safe`) FAIL with the
  host observation "owned children survived teardown": the guest run is
  complete (`probe=boot … END status=PASS`), the QEMU scope did not finish
  its teardown in time while host builds and host suites of the
  integration branch ran concurrently. The same case passed in the
  `f2-process` prefix minutes earlier on this image; it is treated as a
  host-load effect and reruns on the next image. `app-gate` did not run.

## Thirtieth image: `8e8d5dad…` (commit `9de5efc`, normal boot)

Not a campaign: a headless normal boot (no fw_cfg request, `-display none`,
QMP screendumps) of the image built with the blocking VFS lock. The serial
log reaches `[supervisor] bootstrap: spawning /bin/desktop` and
`[supervisor] desktop pid=2 pgid=2`; the desktop, the Ciuki menu, Files,
CiukNote, Task Manager and the DOS window were captured. On the previous
images (`9fbfab4d…` and earlier) the same boot stopped after
`[storage] disk_log=available error=0`: the boot task held the VFS lock
while reading `/bin/desktop` and the storage worker yield-spun on it at
device priority. The DOS-window capture still showed the kernel console
drawn behind the window and a flood of `[work] budget exceeded
site=storage_work` lines; both fixes (console dark while a display grant
is held; report limit) are in the next image.

## Thirty-first image: `e0af9210…` (commit `50ba47f`, version 0.9.0 pre-alpha, all F3/F4 rows and the normal-boot cases merged)

Campaign 12 (night of 2026-10-11/12) ran `f3-workloads` (which carries the
F0–F2 prefix and `f3-vm`), `f4-media` and `f4-audio`; the hardware image
`build/hw/ciukios-hw.img` (SHA-256
`ce1453f18de18f933ab1ba5ce512df8e441b38392ae9af6f21a54f0fe3747850`,
`BOOT.CFG` `safe=0 serial=1`, normal boot) was built from the same tree.

- `f3-workloads`: 185 PASS through the prefix, including the two new
  `normal-boot-qemu-t23` and `normal-boot-qemu-desktop-2002` cases (boot
  without a test request to `[supervisor] desktop pid=`, desktop colours
  verified on the screendump), then `crash-isolation-safe-qemu-min128`
  FAIL "owned children survived teardown" with the guest run complete
  (`END status=PASS`) and `cleanup.qemu_process_count=1`: the runner
  counted the QEMU process in the instant between the wrapper's exit and
  systemd's reaping. The runner now allows up to five seconds for that
  count to reach zero. The remaining prefix cases, `f3-vm` and the
  workload cases did not run; they run again on this image as the scratch
  suite `zz-night` (campaign 13).
- `f4-media`: 4 PASS, then `optical-malformed-qemu-t23` FAIL: the probe
  reports `case=malformed refused=0 result=-5` (the malformed ISO 9660
  fixture produced EIO instead of a refusal); diagnosis assigned, the
  other cases run as `zz-media` (campaign 13).
- `f4-audio`: `audio-native-qemu-t23` and `-qemu-desktop-2002` PASS;
  `vm-audio-qemu-t23` FAIL with `END status=FAIL error=-14` after a VM
  stop with `data_loss=1` (the f4-05 probe and VMPLAY.COM pass in the
  libx86emu guest runner but not under QEMU); diagnosis assigned.

Campaigns 13–17 (same image `e0af9210…`, scratch suites without the
prefix, runner with the teardown grace and the explicit `ide-cd` optical
attach):

- `zz-night` (f3-vm then f3-workloads): `vm-boot`, `vm-memory`,
  `vm-devices`, `vm-video` PASS on qemu-t23/e500/min128 (12 cases);
  `vm-window-qemu-desktop-2002` FAIL: the dosbox client's CREATE returned
  ENOSPC (`dosbox: create: Not enough space` on the console) before any VM
  existed, so the probe timed out (`END error=-110`); diagnosis assigned.
- `zz-media` (f4-media without the malformed optical cases): 18 PASS
  (NTFS twelve cases, optical l1/l2/l3 on both profiles with the explicit
  ide-cd attach, eject/reinsert included).
- `zz-night2`: `vm-audio-state-qemu-t23/e500/min128` PASS (first QEMU
  evidence for f3-05); `dpmi-qemu-t23` FAIL: the DPMI16 client reports
  `FAIL 0D 02` with most rows failing (`ok=0000001ee67c03dfe1ff3fcf
  fail=00001fff1d8fffa3ff67fff7`) under TCG while it passes in the
  libx86emu runner; the probe's screen dump then violated the record
  grammar (same `\`/`>` defect as the VM probe, fix assigned with the
  diagnosis).
- `zz-night3`: `dos4gw-qemu-t23` and `dos4gw-qemu-min128` PASS (20
  launch/exit loops each, resources released); `doom-qemu-t23` FAIL at the
  fixture stage (`case=fixture status=error error=-12`, ENOMEM when the
  160 MiB local fixture disk is attached); diagnosis assigned.
- `zz-night4`: `wolf3d-qemu-t23` FAIL with the same fixture error.
- `zz-night5`: `vm-starve-qemu-t23` and `vm-starve-qemu-min128` PASS.

Not yet exercised on QEMU: the f4-05 `vm-audio` cases with the idle-tail
fix (merged after this image), `doom-sound`, the second DPMI profiles.
