# Test architecture: one image, many runs

Decision, 2026-10-09 (owner request: stop creating a different build for each
test and use the workstation's resources efficiently).

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
5. **Resource limits are built into the runner.** One QEMU at a time
   (a lock file); each run under
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

## Migration

The pre-cleanup test scripts were archived in
`legacy/CiukiOS-scripts-legacy-2026-10-09.zip`. They are a reference for
markers and QMP sequences, not something to restore. New suites are written
for the runner as features are rebuilt; because of the planned architecture
reboot (see `dev_diary/2026-10-09-02-revisione-architettura.md`), the fw_cfg
hook and the runner should be part of foundation phase F0, so every later
phase is tested this way from its first boot.


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
