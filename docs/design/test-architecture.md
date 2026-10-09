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
