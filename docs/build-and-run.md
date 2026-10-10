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
