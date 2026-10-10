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
folder. Record the verified write/readback hashes, embedded build id and all
existing physical identity fields, `operator_confirmed: true`,
`selector_source: "cfg"`, the initial selector, and per-boot
`panic_observations` keyed by one-based boot number (external_halt_seconds,
resumed=false means no spontaneous execution resumed before the power-cycle).
Historical mixed-run captures may explicitly list approved `selectors`.
Never infer an operator observation or checker result from the serial stream.
Import once with:

```bash
python3 scripts/test/run.py f2-all --physical-capture legacy/local/physical/12345678
```

The command imports only and writes one profile=physical summary. Missing
fixture/disk checker/screen/operator evidence remains not_run. The safe-mode
cursor conflict is recorded in f1-acceptance.md; a sweep is not yet a substitute
for the dedicated safe qualification. QEMU suites remain unchanged; the lead
can run `python3 scripts/test/run.py sweep-smoke` to traverse F0 on one bounded,
reused overlay under -no-reboot and recover its final panic.
