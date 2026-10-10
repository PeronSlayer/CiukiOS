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
| Tests | `qemu-system-i386`, `systemd-run` (user scopes with a memory controller) |

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
and watch for `[operator] press A, move the pointer, click (60 s)` on screen
and serial. The first event must arrive within 60 seconds. For `input`, then
complete the normal 120-second stimulus: 100 unshifted A press/release cycles,
100 pointer moves and ten left-button press/release cycles. The probe retains
its existing count/digest/motion checks; being present alone does not pass it.
For the desktop interaction, press and release A, move the pointer and click
and release the left button within its normal 15-second window; observe that
the desktop redraws. With no first event, only `stimulus` or `interaction` is
`status=not_run reason=operator_absent`; setup/lease, completed isolation cycles
and cleanup keep their verdicts. The sweep counts the incomplete step as
not_run and continues. Physical import preserves this reason and marks
`operator_confirmation_required: true`; a case confirmation cannot turn an
absent stimulus into a pass. Record real operator observations in
`case_confirmations` and keep the existing independent screen/disk evidence.

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
| `selector_source` | Multi-boot import defaults to `"cfg"`, single-probe import to `"menu"`. A sweep requires `"cfg"` and each recorded boot must retain `L:SELECT_SOURCE=cfg`. |
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
startup and console lines; unrelated diagnostic lines are omitted, and the
historical fixture keeps only boots 1, 10 and 33.

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
fixture/disk checker/screen/operator evidence remains not_run. The safe-mode
cursor conflict is recorded in f1-acceptance.md; a sweep is not yet a substitute
for the dedicated safe qualification. QEMU suites remain unchanged; the lead
can run `python3 scripts/test/run.py sweep-smoke` to traverse F0 on one bounded,
reused overlay under -no-reboot and recover its final panic.
