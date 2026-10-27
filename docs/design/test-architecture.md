# Test architecture: one image, many runs

Decision, 2026-10-09 (owner request: stop creating a different build for each
test and use the workstation's resources efficiently).

For the normal-boot T23 contention records (`vfs_hold`, `vfs_wait`, ATA queue
mode/service counters, DOS VM perf and `irqoff_long`), see
[f1-86 record interpretation and host coverage](../validation/f1-86-t23-contention-instrumentation.md).

## Problem measured on 2026-10-09

Before the cleanup the working tree held 121 GB, of which 114 GB were
`build/`: 39 GB in `build/tests`, 47 GB of hardware/T23 variants under
`build/full`, 9.5 GB of objects and dozens of 128 MiB image copies. Of the old
scripts, 27 rebuilt the image with build-time flags (`CIUKIOS_STAGE2_AUTORUN`,
`CIUKIOS_STAGE1_SELFTEST_AUTORUN`, `CIUKIOS_INCLUDE_DOS_WINDOW_PROBES`, trace
flags, …), 105 copied whole `.img` files, some 64 GB sparse disks, and only 10
used copy-on-write. Earlier, `/tmp` (a 7.4 GiB tmpfs, that is RAM) filled with
128 MiB `target.img` copies, and looped QEMU runs crashed the 14 GiB host.

## Sources

- [QEMU fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html):
  on x86 the selector is port `0x510` and data port `0x511`; selecting key
  `0x0000` returns the signature `QEMU`; key `0x0019` is the file directory;
  `-fw_cfg name=opt/<RFQDN>/...,string=...|file=...` passes user items, and
  user names must start with `opt/`.
- [QEMU disk images](https://www.qemu.org/docs/master/system/images.html):
  `-snapshot` writes changed sectors to a temporary file in `/tmp`;
  qcow2 overlays reference a backing image and store only changes.

## Rules

1. **One canonical build per source state.** `make build-full` (and
   `make build-full-cd` when the CD matters) produces the only images tests
   use. Tests never invoke `build_full.sh`, never set build-time flags and never
   copy a whole image. Build-time variants are allowed only when the owner
   approves one for a specific reason, recorded in `dev_diary/`.
2. **Test behaviour is chosen at run time, not at build time.** The image
   carries its test probes (they are small) and a boot hook that looks for
   the QEMU fw_cfg item `opt/it.alcybercloud.ciukios/test` (a short command
   line such as `run \TESTS\VMTEST.COM /switch`). Without fw_cfg, as on every
   physical PC, the hook does nothing. Replacing the existing autorun, self-test
   and probe build flags with this hook is the first implementation step.
3. **Copy-on-write, on disk.** Each run creates a qcow2 overlay
   (`qemu-img create -f qcow2 -b <image> -F raw run.qcow2`) under
   `build/test-runs/<suite>/<run-id>/`. Never use `-snapshot` without
   `TMPDIR` on disk, and never write test data to `/tmp`.
4. **One runner, declared suites.** `scripts/test/run.py <suite>` reads a
   suite file from `tests/suites/`. A suite declares the image (`full` or
   `cd`), QEMU profile (machine, CPU, RAM, devices), the fw_cfg command, the
   expected serial markers or screen checks, and a timeout. Common QEMU and
   serial/QMP code lives in one module instead of being copied into each test.
5. **Resource limits are built into the runner.** At most N runner slots
   (owner decision 2026-10-12; N = 2 by default, `CIUKI_RUNNER_SLOTS` 1..4),
   still one QEMU per case. Each runner process takes the first free slot lock
   `build/test-runs/.qemu.slotN` (flock) for its whole run and is refused when
   all are busy. Preflight requires 2 GiB of `MemAvailable` plus 2 GiB per other
   running slot, and refuses while a QEMU outside every runner scope (a manual
   one) or a heavy build exists; a guest inside another slot's
   `ciuki-test-*.scope` is never foreign, and the teardown check counts only the
   case's own scope. `summary.json` is written by atomic rename. Slots are taken
   automatically (no `--slot` option). Evidence semantics (image SHA, overlays,
   TCG flags, records) are unchanged. Each run under
   `systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0`; free
   memory checked first; no `pmemsave` or RAM dumps unless a diagnosis needs
   them; no orphan `qemu-system` processes left behind.
6. **Retention.** Passing runs keep only `result.json` and the serial log;
   the overlay and screenshots are deleted. Failing runs keep their artifacts.
   Only the last 5 runs per suite are kept, and the runner refuses to start
   if `build/test-runs` exceeds 2 GiB until older runs are pruned.
7. **Tiers, cheapest first.**
   - **T0 host:** C and Python unit tests compiled for Linux (seconds, no QEMU).
   - **T1 image:** static checks on the built image (layout, sizes, payload
     manifest, absence of private payloads).
   - **T2 smoke:** one QEMU boot to the desktop/shell on the canonical image.
   - **T3 focused:** feature suites on demand, one at a time.
   - **T4 hardware:** the same canonical image written to a physical disk;
     logs collected by one tool into `legacy/local/` or a dated untracked
     folder, never into the build tree.
8. **Results are evidence only for the image they ran on.** `result.json`
   records the image SHA-256, git revision, QEMU version and profile.

## Normal boot cases (amendment, 2026-10-11)

Every probe case boots with the fw_cfg request, and with
`CBI_F_TEST_REQUEST` the kernel's `probes_main` never calls `drivers_init()`
or `supervisor_bootstrap()`. The path a user boots (drivers, storage with the
boot log and timer, files, audio, supervisor spawning `/Programs/desktop`) was
therefore never covered. A case with `"kind": "normal-boot"` (and
`"probe": "normal-boot"`) closes that gap:

- QEMU gets the same profile, overlay, scope, lock, timeout and teardown as
  a probe case, but no `-fw_cfg` item: the loader menu times out after about
  3 s and boots normally, as on a physical PC. A profile with a `platform`
  override is refused (the override travels in the fw_cfg request). The case
  allows only `id`, `kind`, `probe`, `profile`, `timeout`, `attempt`,
  `expected` and a cold `boot_kind`; `expected` holds only
  `evidence_kind: "normal-boot"`, `settle_seconds` and `screen.geometry`.
- Evidence is serial text, not CIUKI_TEST records. Milestones are whole-line
  prefixes, once each and in this order: `L:CPU signature=`,
  `L:SELECT_SOURCE=menu`, `Ciuki VMM F0 build <id> - CiukiOS`,
  `[supervisor] bootstrap: spawning /Programs/desktop`,
  `[supervisor] desktop pid=<n> pgid=<n>`. The case fails on a line starting
  with `Ciuki VMM panic`/`Ciuki VMM double fault`, any `CIUKI_TEST` record
  (a panic record or a probe record without a request), another
  `L:SELECT_SOURCE=`, `CIUKI: `/`L:SELECT_ERROR`, `L:SAFE=1`,
  `[supervisor] desktop unavailable`, `[supervisor] desktop stopped`, or a
  repeated milestone. The runner keeps collecting for 1 s after such a line
  so the following diagnostic lines reach `serial.log`.
- `settle_seconds` after the desktop line the runner takes one QMP
  [`screendump`](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-screendump)
  (`screen.ppm`) and checks layout colours taken from `apps/desktop` (its
  default theme, Platinum, in `theme.c`), not a frame digest (the taskbar
  clock changes): the bottom 32 rows are mostly the taskbar colour `d6d6d6`,
  the Ciuki button at the left end of the top bar (x 4..100, y 4..28) is
  mostly `f2f2f2` (or `b9b9b9` with the menu open), the top bar right of it
  up to x 256 is mostly `dcdcdc` and has at
  most 2 % kernel console colours (`10243c`, `e8e8e8`, panic `800000`), and
  the area between the bars has at most 2 % of those console colours
  (heuristic 3, 2026-10-12: the default wallpaper is the owner's Ciuk2
  photograph, so the earlier 25 % `375564` desktop-colour requirement no
  longer applies); per-channel tolerance 8. A host test checks these colours against the C
  sources and runs the heuristic on frames from the production compositor.
- `result.json` records `evidence_kind: "normal-boot"` (probe cases record
  `ciuki-test-records`), `selector: null`, `normal_boot` (milestones with
  serial line and host time, negative lines, desktop PID/PGID, kernel build
  ID, settle and screendump times) and `screen` (PPM SHA-256 and the measured
  fraction of every region). Amending rule 6, a passing normal-boot run keeps
  `screen.ppm` beside `result.json` and `serial.log`; a failing run also keeps
  `screen-fail.ppm` taken at teardown.
- `f0-smoke` ends with `normal-boot-qemu-t23` and
  `normal-boot-qemu-desktop-2002` (timeout 300 s, settle 8 s, geometry
  1024x768), so every suite that has `f0-smoke` as a prerequisite stops when
  the image does not boot to the desktop. Amendment 2026-10-25 (QEMU
  `fb12caae`): a normal-boot timeout counts from the loader's source
  selection (`L:SELECT_SOURCE=menu`), and the phase from launch to that
  line has its own bound of 600 host seconds (`NORMAL_BOOT_LOADER_SECONDS`
  in `scripts/test/run.py`). The loader's 3 s boot menu busy-polls INT 16h,
  the COM1 LSR and the BIOS tick without HLT; under the profiles'
  `-icount shift=1,sleep=on` virtual time advances 2 ns per executed
  instruction and at real speed only while the CPU sleeps
  ([QEMU `-icount`](https://www.qemu.org/docs/master/system/qemu-manpage.html)),
  so the menu is about 1.5e9 instructions, which TCG with `-d nochain` ran
  in 270.1-273.9 host seconds (runs `5b7cef8b`, `9948c446`, `fb12caae`).
  Counted from launch, the 300 s budget left 2.8 s after the settle and
  `fb12caae` failed 7.9 s after the desktop started, with no hang.
  `result.json` records `timeout.basis`, `timeout.loader_seconds`,
  `timeout.phase` (`loader` or `system` when the deadline was set) and
  `normal_boot.loader_source_after_seconds`. Physical sweep import reports these
  cases `not_run reason=normal_boot_qemu_only` without blocking later cases: a
  sweep boot always carries a BOOT.CFG probe selection.

F1-31 operator waits apply only to cfg sweep boots, before the normal input
stimulus or post-fault desktop interaction window: announce a screen/serial
prompt, retain the first event, and wait at most 60 seconds. Absence produces
a DATA subcase `status=not_run reason=operator_absent`, followed by
`END status=NOT_RUN reason=operator_absent` only if the other checks passed;
an independent failure retains END FAIL. SWEEP records count/reason and the
physical importer preserves the subcase and operator-confirmation class
without blocking later observations solely for operator absence. Research:
QEMU's [input QMP API](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
defines separate key/button down/up and relative motion events; comparison
with i8042_probe.c, the desktop interaction and console.c confirms that the
runner's existing ARM/stimulus sequence must remain unchanged, while a cfg
sweep needs a first-event deadline and a prompt row refreshed through the
existing console/presenter arbitration. The cfg desktop pixel digest excludes
the prompt's first 16 pixel rows so the prompt cannot qualify as a desktop redraw.
No input events are synthesized and
no QEMU qualification or hardware success is inferred from the host tests.

F1-36 distinguishes an unselected safe-mode sweep step from a failed safe
boot: the required subcase and END are NOT_RUN with
`reason=safe_mode_not_selected`, and SWEEP/SWEEP_END count it as not_run.
Import retains the original observations and reason without blocking later
cases solely for this absent mode; a failed earlier prerequisite still applies.
The separate `safe-menu-<target>` case requires a single menu acquisition with
`selector_source: menu`, all existing safe predicates and the case's operator
confirmation. A sweep cannot qualify that physical case. The QEMU parser and
safe suites retain their PASS/FAIL rules. Source comparison and transport
research are recorded in [F1 acceptance](f1-acceptance.md#f1-28-hardware-sweeps);
the current loader's inability to combine safe selection with a hardware probe
is documented in [build and run](../build-and-run.md#operator-safe-mode-boot-after-a-sweep).

### USB relative-input consumption (runner-03)

Resolve input topology with the existing profile/case override rules. When it
contains a USB `mouse`, retain declared pauses and additionally wait for guest
consumption after every nonzero relative step and button edge. Split relative
axis pairs into requests whose individual deltas are at most 127 units; integer
remainders preserve each axis total and signs. Keep PS/2 and USB keyboard/tablet
only cases on their existing schedule. The action remains incomplete until its
last pointer batch has been consumed, including repeated actions.

Consumption requires a counter baseline and a later observation reaching that
baseline plus the number of nonzero axes (`motion`) and button edges (`buttons`)
sent in the request. Accepted sources are controller DATA records with
`case=gate`, `case=native-desktop`, `case=interaction` or `group=input`, and
anchored kernel `[input]`/`[gate]` lines containing both cumulative counters.
Application frames cannot acknowledge a batch. A missing baseline or stalled
batch refuses after 10 host monotonic seconds, naming action, batch, repeat
and split step; the case deadline remains an independent outer bound.
`result.json` records `input_pacing: {batches, waits_ms}`: the number of actual
QMP input requests and a list of elapsed milliseconds for completed consumption
waits. Declared stimulus hashes remain unchanged; observed stimulus records
retain original batch indices and include the split step.

Research checked against the repository on 2026-10-14:
[QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
injects events without a guest-consumption acknowledgement.
[QEMU 11 HID source](https://github.com/qemu/qemu/blob/v11.0.0/hw/input/hid.c)
merges adjacent pending pointer movements with matching buttons in
`hid_pointer_sync`, and `hid_pointer_poll` drains signed 127-unit chunks.
[USB HID 1.11, Appendix B](https://www.usb.org/sites/default/files/hid1_11.pdf)
defines boot mouse relative axes in the -127..127 range. Repository
`grants.c:desktop_input_event` maps each nonzero axis to a separate public motion
event; `desktop.c:gate_input` counts these consumed events and button edges.
Therefore both axes of an injected pair must be observed before advancing.

### USB counter publication and preflight (runner-04)

`f2_probes_desktop.c:native_report` now forwards the first fully validated
native desktop summary as a controller DATA `case=gate server=desktop keys=…
motion=… buttons=…` record, including the initial zero pointer baseline.
Subsequent summaries emit only when one of those counters changes. This uses
the desktop's existing dirty-input reporting cadence (100 ms), so a delivered
pointer batch gets its consumption record without an extra poll, timer or
input-path log. Unchanged command summaries, demo reports, unrelated processes
and rejected summaries emit no counters. Optional app/checkpoint fields must
also pass validation before publication. Keys retain the gate scenario's
existing success semantics; motion and buttons count consumed public events.
The runner's existing controller-record parser consumes these records.

Before capability queries, overlay creation or QEMU launch, USB mouse cases
that script nonzero relative axes or button edges require an explicit
`f2:crash-isolation … server=desktop` selector, graphics and safe mode off.
Other probes and automatic/stand-in controller selection have no guaranteed
counter publisher and refuse with `USB pointer pacing has no counter record
source`. Profile/case topology overrides still apply; PS/2, keyboard/tablet-only
USB and cases without pointer stimuli need no source. Runtime missing/stalled
records retain the 10-second diagnostic, since preflight cannot prove which
kernel an image contains. Host fixtures model these records and do not qualify
the guest image.

Research rechecked on 2026-10-14 against the
[QMP input-send-event documentation](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
and [upstream HID implementation](https://github.com/qemu/qemu/blob/v11.0.0/hw/input/hid.c).
QMP completion still acknowledges injection only; pending pointer reports can
merge before polling. Repository `desktop.c:gate_input` counts consumption and
`gate_report` already coalesces dirty input at 100 ms; `native_report` validates
identity, monotonic counters and optional checkpoints, while `sys_probe_report`
consumes its summaries. Forwarding the validated summaries is therefore the
cheapest available publisher and preserves the existing input schedule.

## Acceptance cases (amendment, fx-accept, 2026-10-27)

Owner decision 2026-10-27 (`docs/directives/fx-accept.md`): every hardware
image first passes the owner's trial list on the `qemu-t23` profile; a
failing item blocks the image. The suite is `tests/suites/acc-t23.json`
(`"acceptance": true`), run with `scripts/test/acceptance.sh` or
`make qemu-acceptance`; besides `summary.json` the runner writes the one-page
`build/test-runs/acc-t23/summary.txt` (verdict ACCEPTED, BLOCKED or
INCOMPLETE, one line per trial item, key figures and evidence paths).

- **Case kind `acceptance`** (`scripts/test/acceptance.py`): a boot without a
  test request, driven like a user. The runner presses `n` at
  `SELECT_READY` (Normal at once: the menu's 3 s wait costs about 270 host
  seconds under TCG with `-icount`) or F5 (Startup menu item 3, Safe mode,
  `L:STARTUP=3`), then the desktop's own keys (Ctrl+Esc and menu letters,
  Win+R, Alt+Enter, Alt+F4, the Shut Down dialog's R/S), through QMP
  [`input-send-event`](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event).
  Generated ISO images (`scripts/test/iso_fixtures.py`, never real media)
  enter an empty `ide-cd` on the secondary master (the T23 drive position;
  an `-drive if=none` backend without a file) through `blockdev-change-medium`
  and leave through `eject`. A guest RESET is legal only after a step armed
  it and within the case's `resets`; `-no-reboot` is omitted only for such a
  case, and `-no-shutdown` stays, so ACPI S5 pauses QEMU with a SHUTDOWN event
  of reason `guest-shutdown`
  ([QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html)).
- **Predicates** are line-anchored regular expressions over the serial
  capture, never a frame digest: kernel `klog` lines, program output
  (`[console] ...`) and the desktop's diagnostic records
  (`[user N] [desktop] display start|focus|not-responding|notify|message|autoplay ...`,
  `apps/desktop/diag_record.h`, written through diagnostic call 2 also in
  quiet boots) and `[dosvm-window] video` (`src/kernel/vm/dosvm_vga.c`, one
  line per change of a DOS window's layout). Steps wait for patterns in order
  and capture values (`<<name>>`); assertions count lines between steps;
  figures summarize (the `[dosvm] perf` window counters of the T23 logs).
  Installed files and Start-menu links are compared on the overlay after a
  verified teardown (`win32_inventory.overlay_delta`). Screendumps only pace
  the input (`screen_settle`; an installer `wizard` presses its next key once
  the screen has been still for some seconds and the focused window is one
  of the installer's untitled pages) and check geometry and the desktop
  layout colours of `observe_normal_boot_screen`.
- **Known failures** name their directive (`known_failure`, optionally only
  for a `fail` pattern): the summary shows the item as PASS* and they do not
  block. Today: the DOS full-screen entry refused by QEMU's standard VGA
  readback (f3-29) and the empty CD drive that has no letter yet.
- Items are independent: a failing case does not stop the run, except the
  `gate` check of the first boot. One run keeps every case directory of the
  suite (pruning keeps the number of cases plus one); a passing case keeps
  `result.json`, `serial.log` and its evaluated screendumps, a failing one
  also its overlay, generated images and failure screendumps.
- **Browser cases** of the suite are `f6:browser` probe cases with
  `browser_sites`: the owner's site snapshots (never committed) are served by
  the HTTPS fixture as `/site/<n>/`, so no host name enters a tracked file;
  absent snapshots make the case `not_run`. Same-site fetch errors fail it,
  other hosts are recorded.
- A pending item (`pending`: directive, required paths, reason) is `not_run`
  and does not block (item 9 until fx-is5boot is merged).

## Migration

The pre-cleanup test scripts were archived in
`legacy/CiukiOS-scripts-legacy-2026-10-09.zip`. They are a reference for
markers and QMP sequences, not something to restore. New suites are written
for the runner as features are rebuilt; because of the planned architecture
reboot (see `dev_diary/2026-10-09-02-revisione-architettura.md`), the fw_cfg
hook and the runner should be part of foundation phase F0, so every later
phase is tested this way from its first boot.


### Self-test boot (fx-selftest)

Owner decision 2026-10-27: a T23 trial starts with one unattended self-test
boot (loader `T` or Startup menu choice 9). It runs bounded checks of the
hardware-only paths in one ordinary boot, without a test request, a sweep or a
reboot, and writes `C:\SYSTEM\LOGS\SELFTEST.LOG` plus `probe=selftest`
records; `scripts/test/selftest_report.py` summarizes either copy. Contract,
check table and report format: [self-test](self-test.md).

### F1-28: unattended hardware sweep and reset research

A hardware sweep uses one verified image, one run id and one serial capture.
QEMU qualification suites retain one boot per declared case; sweep evidence
is a separate hardware traversal and cannot replace missing stimuli, external
panic observations, fixture coverage or independent disk checkers.

Reset decision (researched before implementation): the production path first
waits a bounded time for 8042 input-buffer bit 1 to clear, sends command 0xFE
at port 0x64 and allows a short port-I/O delay. It then loads a zero-limit IDT
and executes INT3. Intel describes failure delivering #DF as processor shutdown;
a platform reset after shutdown is a board behaviour, not an architectural
promise. If neither mechanism resets the board, the owner power-cycles it.
The durable cursor has already advanced. No storage call follows the crash reset boundary or
panic ARM. An ordinary reboot follows production write/barrier completion.

Sources: [Linux v6.12 x86 reset implementation](https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/reboot.c)
(8042 pulse and invalid-IDT INT3 fallback),
[Intel SDM Volume 3A, Interrupt 8 / Double Fault](https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf),
[QEMU v9.2 i8042 source](https://github.com/qemu/qemu/blob/v9.2.0/hw/input/pckbd.c)
(command 0xFE pulses CPU-reset bit 0), and
[QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html)
(`-no-reboot` exits instead of rebooting; `-no-shutdown` pauses on shutdown).
The sweep-smoke runner retains -no-reboot, omits -no-shutdown, and relaunches
only its existing overlay, with a bounded boot count. Panic is observed for
five seconds before the runner simulates the owner's recovery power-cycle.


Sweep captures are bounded to 4 MiB (acquisition metadata stays <=128 KiB).
Import splits on L:CPU loader banners, including the leading UART noise seen
in the real T23 capture; SELECT_READY is the historical fallback. It validates
original record bytes, run identity, monotonic sequences within each boot and
the build identity of every boot. Sequence resets are permitted only at a new
loader banner. Failed menu boots contribute no probe evidence. It routes each
probe to the unchanged suite-case predicates with profile physical, retaining
missing subcases, prerequisites, per-case operator confirmations and five-second
panic observations. Disk/screen checker requirements cannot be satisfied by
serial alone. `--physical-capture` imports evidence without launching QEMU or
requiring its executable. One summary retains later observations even when a
failed prerequisite makes their qualification not_run.

F1-35 physical import accepts damaged cfg provenance only through adjacent
sweep boots with the same run/phase and a step increment of exactly one (or
the repeated step of an armed two-boot probe); it records the raw line,
`provenance: damaged` and `provenance_inferred_from: sweep continuity`.
A missing line needs serial damage, including a damaged loader provenance
marker, damaged records or an initial sequence gap. Without continuity the
boot is `not_run reason=provenance_damaged`; an intact alternate source refuses
the acquisition. QEMU-only device exceptions and machine variants require
matching operator acquisition metadata, otherwise their cases are separately
identified as `not_run reason=device_exception_not_applicable:<key>=<value>`
without blocking later cases. Research: the official
[QEMU invocation documentation](https://www.qemu.org/docs/master/system/invocation.html)
defines `-vga none` as disabling VGA and `-serial none` as suppressing default
serial devices, and blkdebug/cache options describe host block configuration.
Comparison with run.py's command construction and the real T23 startup bytes
therefore requires explicit physical configuration matching, unchanged
predicates and byte-preserving provenance evidence, never an inferred device
absence from a suite profile.


Before either reset mechanism, a bounded UART drain waits for LSR bit 6
(TEMT). The existing serial writer waits only for THRE (bit 5), so the final
CRLF might otherwise still be in the shift register at reset. Source:
[Texas Instruments PC16550D SNLS378C, section 8.6.4 LSR](https://www.mouser.com/datasheet/2/405/pc16550d-443503.pdf)
(original vendor datasheet mirrored by its distributor). A disabled serial
sink is not accessed. Each sweep boot first emits `probe=sweep event=BEGIN`
after driver activation, opening the existing production record guard; this
is required even on a recovery-only boot that emits only SWEEP/SWEEP_END.


F1-28 implementer validation (host evidence only):

- `python3 scripts/build_kernel.py`: PASS, ELF link and FPU/SIMD audit.
- `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`: PASS;
  production storage harness 44,305 checks, 253 records, max 209 bytes,
  failures=0; BOOT.CFG post-shutdown round trip/refusal/removal and record
  guard recovery-only scope included.
- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host`:
  189 tests, OK, 7 skips; synthetic three-boot/panic import, actual local T23
  boundary validation, cfg provenance, bounded relaunch and reset order.
- Full image builder requires its existing `--kernel build/f0/VMM.ELF`;
  construction is unavailable in this worktree because
  `build/tools/ciuki-sdk/manifest.json` is missing. No alternate image was made.
- NASM plus the unchanged `build_image.prepare_loader` T1 check: PASS,
  24,576 bytes / 48 sectors (limit 1,023), loader header and CRC32 valid.
  This is loader-only evidence, not a full image/geometry/fsck T1 result.
- No QEMU or physical sweep validation was run by the implementer. The lead
  still owes f0-smoke, f1-safe, sweep-smoke and the T23 sweep/import evidence.
