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
