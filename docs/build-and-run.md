# Build and run (Ciuki VMM, F0)

The canonical image is the 512 MiB FAT32 HDD image `build/f0/ciukios.img`
built from the Ciuki VMM kernel (`src/kernel/`) and the F0 loader
(`src/boot/mbr.asm`, `src/boot/ciukldr.asm`). The 0.8 image is built only on
branch `legacy-0.8` (`docs/design/foundations-transition.md`).

## Dependencies (Linux host)

| Purpose | Tools |
| --- | --- |
| Kernel and loader | `clang`, `ld.lld`, `llvm-objdump`, `nasm` (majors pinned in `config/toolchain.json`) |
| Image | `python3`, `mkfs.fat`, `fsck.fat`, `mtools`, `qemu-img` |
| DOS image DPMI clients (f3-04) | Open Watcom 2.0 under `/opt/watcom` (`binl64/wcl386`, `binw/dos4gw.exe` with the SHA-256 pinned in `scripts/build_dos_image.py`) |
| Optional host guest runner | `libx86emu` (headers and library); skipped when absent |
| Tests | `qemu-system-i386`, `systemd-run` (user scopes with a memory controller) |
| Optical fixtures and independent extraction | `xorriso` |

## Commands

```bash
make build-full        # kernel (build/f0/VMM.ELF) + image, with T1 checks and build-manifest.json
make test-host         # T0: kernel library, loader statics, runner, fixtures, loader model
make qemu-test-full    # T2: f0-smoke through scripts/test/run.py
python3 scripts/test/run.py f0-core    # T3: ten-boot matrix and the nine non-fatal probes
python3 scripts/test/run.py f0-panic   # T3: destructive panic cases
make qemu-run-full     # interactive boot (scripts/run_f0.sh "f0:boot run=00000001" to pass a request)
```

`scripts/test/run.py` takes the common lock, refuses a second QEMU, runs each
case under `systemd-run --user --scope -p MemoryMax=1500M -p MemorySwapMax=0`
on a qcow2 overlay in `build/test-runs/<suite>/<run-id>/`, and writes
`result.json` plus the raw serial log. Profiles are in `tests/profiles/`,
suites in `tests/suites/` (`docs/design/test-architecture.md`,
`docs/design/f0-acceptance.md`).

Interactive runs use TCG by default; KVM is only for the `qemu-fast`
developer profile and never for evidence.

## NTFS host fixtures (f4-03)

Install ntfs-3g's `mkntfs`, `ntfscp`, `ntfsls`, `ntfscat`, `ntfstruncate`
and `ntfsinfo`, plus its shared `libntfs-3g`. No root privileges or filesystem
mounts are required. The library is used only by the generator for mkdir,
unlink, interleaved writes and DOS aliases, which the installed CLI lacks;
it is never linked into the Ciuki kernel.

```bash
python3 scripts/test/ntfs_fixtures.py --out build/host/ntfs
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -p test_ntfs_ro.py -v
```

The test creates fixtures when the generator changes; cached inputs must match
their SHA-256 manifest. It compiles the production parser for the host and
runs a separate ASan/UBSan walk. Listings and streamed file hashes come from
independent ntfs-3g tools, including the >4-GiB file (no whole-file allocation).
A second generation verifies deterministic image hashes and is deleted.
`--entries` accepts 1000–10000; the canonical host set uses 2048 entries in
one directory. Images, temporary inputs, binaries and manifests stay under
`build/host/ntfs/`; the generator refuses output outside a build subdirectory.
Delete that directory when its investigation closes; tests regenerate it.

These are generated nonboot data volumes, separate from the canonical Ciuki
image. The parser currently has no VFS integration or runtime selector.
Bounds, refusal reasons, license references and qualification limits are in
[F4 acceptance, f4-03](design/f4-acceptance.md#f4-03).

### F4 optical fixtures

`scripts/test/iso_fixtures.py` generates deterministic data CDs, without Rock
Ridge, at ISO 9660 Levels 1–3 with optional Joliet. It fixes timestamps and
ownership, declares source hashes and extracts the resulting image with
`xorriso -osirrox` to obtain independent on-media names, sizes and SHA-256
hashes. Source/extraction scratch trees are removed before it returns. For a
host-only fixture or deliberately malformed root extent:

```bash
python3 scripts/test/iso_fixtures.py --out build/host/optical --level 3 --joliet
python3 scripts/test/iso_fixtures.py --out build/host/optical --level 3 --malformed
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -p test_optical.py -v
```

The fixture image and its manifest remain in the requested build directory.
The production parser/VFS host harness checks all six Level/Joliet combinations,
synthetic multi-extent records, malformed descriptors/extents, stale mount
identities and zero block writes/flushes. Native file and mocked ATA packet
checks are also part of `make test-host`.

The lead runs `python3 scripts/test/run.py f4-media` after the canonical build.
Its eight cases cover lawful Levels 1–3 and malformed media on `qemu-t23` and
`qemu-desktop-2002`. A CD occupies IDE index 3 (secondary slave); at most two
FAT fixtures occupy indices 1 and 2, leaving index 2 as the same-channel ATA
peer. Colliding fixtures are refused. `f4:optical run=<8 hex>` is the runtime
selector, including the loader menu. At the probe's `media_reinsert` ARM the
runner issues QMP eject/change for `ciuki-optical` using the generated image.
It compares both directory/hash cycles against the independent manifest and
checks the fixture image hash is unchanged. Passing run artifacts follow the
runner's ordinary cleanup rules. A missing packet device records
`not_run reason=device_absent`; this does not qualify an optical case.

## Test requests

The loader reads the QEMU fw_cfg item `opt/it.alcybercloud.ciukios/test`
(`-fw_cfg name=opt/it.alcybercloud.ciukios/test,string="f0:all run=0000abcd"`).
Grammar: `f0:<probe|all> run=<8 hex> [platform=e500]`; `platform=e500` is
accepted only from fw_cfg and forces the firmware-first input policy. On a
physical PC the loader menu (`P`) accepts the same request. Without a request
the kernel boots to its scaffold prompt on COM1 (38400 8N1) and the screen.

## Physical machines

Write `build/f0/ciukios.img` to an expendable disk with
`sudo scripts/test/write_physical.sh /dev/disk/by-id/<stable-name> [image]`:
it refuses the root disk, partitions and mounted disks, asks for the disk's
exact serial number, writes with `dd`, reads the image back and compares
SHA-256, and leaves a JSON record beside the image. The evidence rules are
in `docs/design/f0-acceptance.md`. For a screen-only run choose `P` in the
loader menu and type `f0:core run=<8 hex>`: every probe but `panic` runs,
then the screen pages through the recorded records (8 s per page) for
photographs. With an RS-232 to USB adapter and a null-modem cable on the
laptop's serial port, `scripts/test/serial_capture.sh <run-id>` records
COM1 (38400 8N1) into `legacy/local/physical/<run-id>/serial.log`; start
it before power-on, then `f0:all run=<run-id>` also covers `panic`.


### Unattended hardware sweep

Before the single canonical build/write, supply a bounded BOOT.CFG containing
ordinary options and this line (choose one eight-hex-digit run id):

```text
safe=0 serial=1
probe=all:sweep run=12345678
```

Use the image builder's existing `--boot-cfg` argument; this changes runtime
configuration, not build-time probe variants. Verify/write/read back that image
once using the existing physical write procedure. Start the serial capture
before power-on. No P entry is required; N/S can cancel the configured request
in the three-second loader window. Normal steps and cold-reopen/cut steps reset
automatically. After an intentional panic or hang, observe the halted screen
for at least five seconds and power-cycle the machine. The cursor continues;
panic at the end of an F0-only sweep requires this recovery boot for SWEEP_END.
Do not edit step/state counters by hand. Completion removes only the probe line.
Read-only cursor refusal leaves evidence on screen and does not reset.

For the `input` and real-desktop `crash-isolation` steps, stay at the machine
and watch for the `[operator]` prompt on screen and serial. The first event
must arrive within 60 seconds. For `input` (f1-34, `criteria=operator`): press
and release A at least ten times, move the pointer, click and release the
left button at least once, then release everything and pause for one second;
the probe stops after the thresholds and one quiet second, or at its
120-second deadline. Loss, duplicates, stuck keys and the input lease are
still checked; typematic repeats from a held key are recorded, not failures;
being present alone does not pass it. For the desktop interaction, press and
release A, move the pointer and click and release the left button within its
normal 15-second window; observe that the desktop redraws. With no first event, only `stimulus` or `interaction` is
`status=not_run reason=operator_absent`; setup/lease, completed isolation cycles
and cleanup keep their verdicts. The sweep counts the incomplete step as
not_run and continues. Physical import preserves this reason and marks
`operator_confirmation_required: true`; a case confirmation cannot turn an
absent stimulus into a pass. Record real operator observations in
`case_confirmations` and keep the existing independent screen/disk evidence.

### Operator safe-mode boot after a sweep

The sweep's `safe` step normally boots with `safe=0`: its durable cursor needs
volume writes. It preserves its observations but records the required subcase,
END and SWEEP as not_run with `reason=safe_mode_not_selected`. It does not
qualify `safe-menu-t23` or `safe-menu-e500`, and the importer does not treat
this absent mode as a failed prerequisite for later cases.

After SWEEP_END, the on-disk `probe=` line has been removed; keep the disk and
the same image. Start a **separate** capture with a fresh eight-hex-digit run id
before powering on. In the three-second `SELECT_READY` window, press **S**
(upper- or lowercase, laptop keyboard or COM1). The loader cancels any configured
probe request, sets safe mode, prints `Safe mode selected; P for a probe, or wait`
and `L:SAFE=1`, and starts a fresh three-second menu window. Within that new
window, press **P**, type **`f1:safe run=<fresh id>`** (replace `<fresh id>` with
the chosen eight hex digits), and press **Enter** within the selector's
30-second entry deadline. The exact sequence is **S, P,
`f1:safe run=12345678`, Enter**, with a fresh id for each acquisition and no
`safe=1` suffix. The loader passes both the safe flag and the selector to the
kernel, and records `L:SELECT_SOURCE=menu`.

After S, expiry or N boots safe mode without a probe; N preserves the safe flag.
L can still toggle serial logging before P; keep logging enabled for this
acquisition. P alone does not add safe mode, and the ` safe=1` selector suffix
remains QEMU-only. Research and source comparison for the bounded menu restart
are recorded in [the boot contract](design/boot-memory.md#sources); this is the
F1-37 loader flow, with host checks distinct from the lead's QEMU and physical
qualification.

Observe `[safe] Console ready.` and the input/console evidence, then retain the
original serial bytes in that run's own capture directory as `f0.log`. Its
`acquisition.json` must contain the verified image/build identity,
`selector: "f1:safe run=<fresh id>"`, `selector_source: "menu"`,
`operator_confirmed: true`, and `case_confirmations: {"safe-menu-t23": true}`
(or the E500 id), confirming the actual operator observation. Qualification
requires END PASS, `safe_mode=1`, `source=menu`, zero optional activations and
mounts, and working console/input; an operator confirmation cannot replace those
records. A synthetic acquisition exercises the importer but establishes no
physical qualification.

The scoped importer can evaluate the declared physical case by passing it in
the `cases` argument of `physical.import_sweep`, together with the suite's
`physical_cases`. The current `run.py --physical-capture` summary enumerates
only the suite's QEMU `cases`; the lead must route/retain the separate physical
menu case there before the command-line procedure can qualify it. Do not merge
the operator boot into the sweep acquisition or relabel a QEMU safe case as
menu evidence.

Place the serial bytes as `f0.log` beside `acquisition.json` in the capture
folder (copy `serial.log` byte for byte; keep CRLF and UART noise). The JSON
object has the following schema. Inventory fields may contain `"unknown"`
when unmeasured; hashes, selectors, confirmation and build identity must be
verified, never inferred from the importer's current checkout.

| Required key | Value |
| --- | --- |
| `model` | Machine model, string; used for physical target predicates. |
| `unit_identity` | Operator-recorded identity of the particular unit, string. |
| `bios_version` | BIOS version, string. |
| `cpuid` | Measured CPU signature, hex string. |
| `installed_ram` | Installed memory, string with units. |
| `pci_ids` | Array of PCI identity strings, or `"unknown"`. |
| `target_disk_identity` | Identity of the disk written and read back, string. |
| `write_sha256`, `readback_sha256` | Matching 64-character lowercase SHA-256 strings for the exact image extent. |
| `capture_settings` | Serial/screen acquisition settings, string; serial uses 38400 8N1. |
| `operator_confirmed` | Boolean `true`, confirming model/unit and acquisition identity. |
| `build_id` | Exact embedded human build identity, e.g. `abcd67745e5f`; match every kernel banner, including any `-dirty` suffix. `embedded_build_id` is not an alias. |
| `selector` | Initial request, e.g. `all:sweep run=66666666`; alternatively supply `selectors`. |

| Optional or conditional key | Value |
| --- | --- |
| `selector_source` | Multi-boot import defaults to `"cfg"`, single-probe import to `"menu"`. A sweep requires `"cfg"`; damaged boot provenance follows the continuity rule below. |
| `devices` | Optional object declaring the physical configuration, e.g. `{"vga":"none"}`. Every case `device_exceptions` key/value must match explicitly; an absent declaration does not prove an absent device. |
| `machine_variants` | Optional object declaring matching case `fault` or `disk_cache` configuration. Values must match exactly, including all fields of a fault object. Never copy QEMU settings into acquisition metadata without physical evidence. |
| `selectors` | Nonempty array of explicitly approved requests for historical mixed-run captures; replaces `selector`. |
| `panic_observations` | Required to qualify each intentional panic: object keyed by the one-based boot number in the complete capture, e.g. `{"10":{"external_halt_seconds":10,"resumed":false}}`. Halt duration must be at least 5 seconds; `resumed:false` means no spontaneous execution resumed before the power-cycle. |
| `external_halt_seconds`, `resumed` | Legacy single-panic observation, or sweep fallback when no per-boot entry exists. |
| `case_confirmations` | Object mapping suite case ids to boolean `true`; required for operator-confirmation cases. Model confirmation does not confirm a case. |
| `disk_log` | Defaults to `"unavailable"`. Any other value requires `storage_qualified:true`; serial import still cannot replace independent disk/screen checks. |
| `storage_qualified` | Boolean, defaults to `false`. |
| `suite`, `utc_start`, `utc_end`, `image_size`, `build_manifest_hash`, `build_git_revision`, `build_dirty`, `runner_revision`, `external_seconds` | Optional single-probe provenance fields retained in its result; omitted values are `"unknown"`. Sweep import retains the entire acquisition object. |

Metadata is bounded to 128 KiB and sweep captures to 4 MiB.
Never infer an operator observation or checker result from the serial stream.
Import once with:

```bash
python3 scripts/test/run.py f2-all --physical-capture legacy/local/physical/12345678
```

For a historical image, explicitly select its verified hash:

```bash
python3 scripts/test/run.py f2-all --physical-capture legacy/local/physical/44444444 \
  --image-sha256 cb33aec38d502e57b966bca55ab0bad59a90f429b8d841e76ba85f9b93481a09
```

`--image` can locate the current canonical image in another worktree. The
override must match both acquisition hashes; it preserves the current canonical
hash, selected historical hash and mismatch in the summary, and does not relax
build/run/probe verification. It applies only to physical import.

The splitter uses the earliest surviving startup stage in each boot: `L:CPU`
(even if its diagnostic fields lost bytes), `SELECT_READY`, selector provenance,
then the kernel build banner. Later stages in that startup do not split again;
a sequence reset alone never establishes a boot boundary. Import uses original
bytes and CR/LF framing. Research: Python's
[bytes.splitlines documentation](https://docs.python.org/3/library/stdtypes.html#bytes.splitlines)
defines CR, LF and CRLF boundaries; unlike `str.splitlines`, it does not split
at form feed. This preserves the real capture's serial bytes and boot numbering.
Sequence gaps and damaged controller records become per-boot `records_lost`,
sequence ranges and original damaged bytes in hex. If an envelope is damaged,
the loss count is at least the number of unusable records; no missing identity
or sequence is invented. Such boots remain unqualified; duplicate/decreasing
sequences and identifiable wrong identities refuse
the acquisition. The anonymous host fixtures retain original controller,
startup and console lines; the earlier fixtures omit unrelated diagnostic
lines, and the historical fixture keeps only boots 1, 10 and 33. The local
`77777777` fixture retains all 35 boots byte for byte; its ignored `f0.log`
is not published, and the real-capture tests skip when the log is absent.

An intact `L:SELECT_SOURCE=cfg` retains the existing provenance rule. A damaged
value such as `fg`, or a damaged marker such as `L:ELECT_SOURCE=cfg`, qualifies
only when the immediately previous verified sweep boot has the same run id
and phase and the intact `probe=sweep event=BEGIN step=<n>` advances by exactly
one. A repeated step qualifies only for the same two-boot probe (`fat-write`,
`mount-crash`, `bootlog`) after its prior reboot/cut ARM. A completely missing
line additionally requires a damaged controller record or a sequence gap
starting at 1. Import retains `provenance: "damaged"`, the raw line and its
hex bytes (null if absent), and `provenance_inferred_from: "sweep continuity"`
in `boot_evidence`. Without continuity, that boot's results remain
`not_run reason=provenance_damaged`. An intact alternate source (`menu`,
`fw_cfg`, `none`), or a clean boot missing provenance, refuses the acquisition.
Record loss still prevents qualification even when provenance is inferred.

Cases requiring an undeclared or different physical device/machine variant
are listed separately by `device_exceptions_not_applicable` and
`not_run reason=device_exception_not_applicable:<key>=<value>` in the summary's
case entries. For example, `video-fallback` requires `devices: {"vga":"none"}`;
a T23 with an LFB does not satisfy its QEMU text-mode predicates. Matching
acquisition declarations enable the unchanged predicates; they do not supply
missing operator, disk or screen evidence. Inapplicable variants do not block
later cases as failed prerequisites. Sources and implementation comparison:
[QEMU invocation](https://www.qemu.org/docs/master/system/invocation.html)
defines the VGA/serial absence and host block settings used by run.py;
[Python bytes.splitlines](https://docs.python.org/3/library/stdtypes.html#bytes.splitlines)
supports retaining the original damaged loader line bytes without text repair.

The command imports only and writes one profile=physical summary at
`build/test-runs/<suite>/summary.json`. Each case retains direct
`evidence_outcome`/`evidence_reason`, original sweep completions and
`not_run_reasons`, plus the prerequisite decision used for its qualification
`outcome`. A failing prerequisite marks subsequent cases not_run while keeping
their direct results. The sweep's reported totals are retained separately;
they are not a claim that suite predicates or physical qualification passed.
Exit 0 means all cases qualify and the sweep completed, 1 means evidence was
imported with failed/not_run cases or an incomplete sweep, and 2 means refusal.
Missing
fixture/disk checker/screen/operator evidence remains not_run. The normal sweep
safe step is `not_run reason=safe_mode_not_selected`; dedicated safe menu
qualification uses a separate acquisition as described above. QEMU suites remain unchanged; the lead
can run `python3 scripts/test/run.py sweep-smoke` to traverse F0 on one bounded,
reused overlay under -no-reboot and recover its final panic.
