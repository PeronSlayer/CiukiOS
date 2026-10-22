# f5-15C — the QEMU `installers` tier (2026-10-21)

Directive `docs/directives/f5-15c-qemu-installers-tier-guest-ui-driver.md`.
This record describes what the tier does, how the lead runs and calibrates it
(one command per title), the decisions taken and what is still open. No QEMU
run, image build or hardware run was made by the implementation; every
result below is host evidence. The lead runs the tier.

## Architecture

| Piece | Where | What it owns |
| --- | --- | --- |
| Probe tokens | `src/kernel/probes/selector.c`, `scripts/test/loader_model.py`, loader patch `build/codex/f5-15c-menu-inc.patch` | `f5:win32-<alias> run=<id>`, the ten aliases of `tests/win32/workloads.json` |
| Kernel observer | `src/kernel/probes/installer_title_probe.inc` (registered in `f2_probes_desktop.c`) | identity, segments, root process start/exit with the Win32 exit status of the zombie, the C: inventory delta (paths, sizes, SHA-256, link targets) against a snapshot taken when w32srv answers, install/uninstall summaries, END |
| Guest UI driver | `apps/desktop/installer_title.c` | runs the installer from the CD through the desktop: w32srv window directory (HWND, PID, parent, style, caption, class) for matching, `desk_input` keys/clicks, interactive defaults, virtual-restart and crash dialogs, Add/Remove Programs uninstall |
| Operator program | `tests/win32/operators/<id>.json` `guest` block, compiled by `scripts/win32/operator_program.py`, parsed by `src/probes/win32_installers/title_program.h` | per-title steps, defaults, restart answer, uninstall, timeouts |
| Checkpoint page | `src/probes/win32_installers/title_controller.h`, VA `0x10100000` (4 pages) | desktop→kernel phases (`READY`, `STARTED`, `UI`, `ROOT_EXITED`, `INSTALLED`, `UNINSTALLED`, `DONE`) |
| Runner media | `scripts/test/win32_fixtures.py` sources `download`, `disc`, `run` | the installer on a generated ISO, or the original disc (the verified ISO, or the data track derived from a verified BIN/CUE zip); the FAT16 parameter disk with `CIUKI-ID.TXT` and `CIUKI-OP.TXT` |
| Host inventory | `scripts/test/win32_inventory.py` | canonical C: versus the overlay's C: after teardown (FUSE read-only slice): exact file/directory delta with SHA-256, CRG1 registry hive delta, shell-link targets |
| Runner rules | `scripts/test/win32_runner.py`, `scripts/test/run.py --win32-calibrate` | Win16 deferral, calibration waivers, host checks, calibration write |

The guest pauses `shot_ms` (1.5 s) after every `ui` record; the runner takes
a QMP screendump `ui-<seq>.ppm` for each. The interactive-defaults method
presses, on a settled page (4 s without a window change), the first enabled
button whose caption matches Finish, Install*, Next*, Yes, I Agree, Agree,
Accept, I accept*, OK, Continue, Close, Done, Start*, Setup — by its
mnemonic (Alt+letter, which the user32 dialog manager turns into
`BM_CLICK`), else Enter (the default push button); never Cancel, Back, No,
Browse. A page with no such button (a progress page) is left alone. An
installer that ends before the program's mandatory steps fails with
`steps_left` (the program no longer describes the run: recalibrate); an
optional step that never matches is skipped after its timeout. A link file in
another format than the CiukiOS v3 link (an installer's own MS-SHLLINK file)
is recorded with the target token `<foreign>` by both the kernel and the
host inventory.

## One command per title

```sh
nice -n 19 python3 -B scripts/win32/titles/<id>.py --tier installers                  # plan
nice -n 19 python3 -B scripts/win32/titles/<id>.py --tier installers --calibrate install
nice -n 19 python3 -B scripts/win32/titles/<id>.py --tier installers --calibrate full
nice -n 19 python3 -B scripts/win32/titles/<id>.py --tier installers --accept
nice -n 19 python3 -B scripts/win32/titles/<id>.py --tier installers --run
```

`<id>` is one of `diablo-shareware`, `starcraft-shareware`, `aoe-trial`,
`half-life-uplink`, `ut-demo-348`, `heroes3-demo`, `rct-demo`,
`jazz2-shareware`, `ta-demo`; `worms-armageddon-demo` reports
`not_run deferred_win16` (`deferred (Win16)`). Each command runs one case of
`tests/suites/f5-installers.json` through `scripts/test/run.py` (one QEMU,
the runner's capped scope, a fresh overlay). An absent payload ends
`not_run fixtures_absent: verified workload archive absent: <path>` before
any boot.

## Calibration procedure (lead)

1. Merge `main` (f0-54) and apply `build/codex/f5-15c-menu-inc.patch`; empty
   `MENU_PENDING` in `tests/host/win32/test_probe_registry.py` and the
   `KNOWN_UNREGISTERED` entries of `tests/host/test_runner_selectors.py`;
   `make build-full`.
2. `--calibrate install`: the parameter disk carries the program with the
   uninstall segment disabled. The run directory keeps `ui-*.ppm`,
   `screen-*.ppm`, `calibration.json`, `serial.log` and the overlay; the
   runner writes `installed_files` (`status: calibrated`: files with sizes
   and SHA-256, directories, changed and removed files, links with targets,
   the registry delta, the run/image/payload binding, the region digests of
   the case's screens) into `tests/win32/workloads.json`. The outcome is
   `not_run` by construction: a calibration never passes a gate.
3. Review the frames. When the defaults did not complete the installer (a
   bitmap launcher such as Diablo's `AUTORUN.EXE`, a licence radio button,
   a directory page), add explicit `steps` to the title's `guest` block
   (`click` with coordinates read from the frames, `button`, `keys`,
   `text value=... replace=1`, `expect text=C:\Program Files\...` as the
   explicit directory answer) and repeat step 2.
4. `--calibrate full`: the same installer flow plus Add/Remove Programs
   uninstall when the installer registered an entry; the runner records
   `installed_files.uninstall` (`registered`, the exact residue from the
   host delta of the final state).
5. Review `git diff tests/win32/workloads.json`, then `--accept`: the manifest
   becomes `verified`, the `guest.status` `qualified`, the calibrated region
   digests become the case's screen pins, and the case's `uninstall` flag
   follows the registration. Set `win32.edition` and, where calibration
   showed them, the installer's documented exit codes
   (`installer_exit_codes`); add a checkpoint for `install-complete`
   (`{"event":"DATA","case":"ui","segment":"install","id":"<n>"}` of the
   Finish page) if that screen is to be asserted.
6. `--run`: the asserting run. `installed_files` holds when the kernel's
   inventory equals the manifest and, without uninstall, the host delta
   equals it exactly; `uninstalled_files` holds when the host delta after
   uninstall equals the recorded residue.

## Lead decisions applied (directive item 1)

- SHA-256 pins promoted from the f5-15B receipts into
  `tests/win32/workloads.json` (ten approved rows; `source.sha256` left the
  `unknowns`); `tests/host/test_win32_workloads.py` asserts the exact pins
  and, where the lead's receipts exist locally, that they agree.
- `ntdll!_vsnprintf`/`_vsnwprintf` implemented (`sdk/win32/ntdll/printf.c`)
  with the semantics of Wine 10.0 `dlls/ntdll/printf.c` and `printf.h`
  (https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/ntdll/printf.c,
  https://raw.githubusercontent.com/wine-mirror/wine/wine-10.0/dlls/ntdll/printf.h):
  partial copy and −1 on overflow, no terminator on an exact fit, NULL-buffer
  counting, the h/l/w/ll/I64/I/I32/z/t/j prefixes, `%n`, `(null)`, `%p` as
  zero-padded upper-case hex, no floating point (the conversion character is
  copied), positional `$` refused. Host test
  `tests/host/win32/ntdll_printf_test.c` (31 checks, ASan/UBSan).
- `config/wine-upper.json` refreshed (`wine_upper.py refresh-audit`): only
  the `ntdll.dll` core hash changed.
- `newer-subsystem-version`: `sdk/win32/ldr/pe.c` read the right field
  (`MajorSubsystemVersion`, optional header +48, 16-bit;
  https://learn.microsoft.com/en-us/windows/win32/debug/pe-format,
  "Optional Header Windows-Specific Fields"). Measured: every acceptance
  installer and every CiukiOS-linked PE declares 4.0; the reused Wine
  providers (`comctl32`, `shlwapi`, `shell32`, `msvcrt`, `comdlg32`,
  `crtdll`, `dplayx`, `dsound`, `advpack`, `cabinet`, `setupapi`, …) declare
  6.0. The unattributed record (`module=image`) came from those builtins at
  every comctl32 attach. The parser now keeps the version
  (`subsystem_major/minor`); the loader records
  `[win32] load check=newer-subsystem-version module=<image>` once per
  non-builtin image (the contract's "accepted with a log record"); builtin
  store images are covered by the store manifest; the kernel seed parser's
  duplicate `warning=newer-subsystem-version` line is removed. Host test
  `tests/host/win32/test_loader.py::test_f5_15c_subsystem_version_record_names_application_image`
  (real lld-link 4.0 headers and the 6.0 corpus image).

## Sources for the media and inventory code

- Microsoft FAT specification (BPB, FAT type by cluster count, long names
  and their checksum): https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
- ECMA-130 (CD-ROM sector: 12-byte sync, header, Mode 1 user data at +16)
  and the CD-ROM XA Mode 2 Form 1 layout (8-byte subheader, user data at +24):
  https://www.ecma-international.org/publications-and-standards/standards/ecma-130/
- The CRG1 hive text format and CRC: `apps/w32srv/storage.c`,
  `apps/w32srv/registry.c` (`wr_crc` is the reflected CRC-32 of zlib).
- The CiukiOS v3 shell link: `apps/desktop/shell_link.h`.

## Known limits and risks for the first QEMU runs

- Window positions of child controls are not in the window directory; a
  click needs coordinates from the calibration frames.
- An installer started from the CD sees `D:`/`E:` paths (argv[0] in DOS
  form, cwd on the CD); the native-path launch convention of the desktop
  shell (`desk_w32_native_path`) still refuses non-C: targets, so CD
  launches go through the driver's own spawn.
- The kernel inventory walks the whole C: tree twice (snapshot, delta) and
  hashes every new or changed file inside the probe-report call; for the
  largest titles this takes tens of seconds under TCG. The segment budget is
  480 s (install 330 s, uninstall 150 s by default) inside the 600 s case.
- The exclusion list (`title_path_class`, mirrored in `win32_inventory.py`,
  equality tested) covers the hive slots, the restart journal and boot logs;
  the first calibration shows any other file the canonical boot rewrites.
