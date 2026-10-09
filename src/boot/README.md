# F0 HDD boot path

The binding contracts are docs/design/boot-memory.md and
docs/design/f0-acceptance.md. boot_info.inc and ciuki/boot_info.h remain
the frozen v1 handoff.

## Disk and header

The 512 MiB sparse image has an MBR at LBA 0, CIUKLDR at LBA 1–1023,
zero padding through LBA 2047, and one active type 0Ch partition at LBA
2048. FAT32 uses 512-byte sectors, eight sectors per cluster, two FATs,
32 reserved sectors, FSInfo 1, backup boot 6 and backup FSInfo 7.
Only SYSTEM/VMM.ELF and SYSTEM/BOOT.CFG are installed.

The packed little-endian loader header is:

| Offset | Type | Value |
| --- | --- | --- |
| 0 | char[4] | CLDR |
| 4 | u16 | version 1 |
| 6 | u16 | padded image size in 512-byte sectors |
| 8 | u32 | IEEE CRC32 of bytes [16, sectors*512) |
| 12 | u16 | real-mode entry offset |
| 14 | u16 | header size, 16 |

The builder patches size/CRC and a stable CIUK MBR disk signature.
The MBR relocates to 0000:0600, checks EDD, reads one sector at a time
and verifies CRC before jumping to 2000:entry. Without EDD it obtains
BIOS geometry and uses CHS: loader LBAs never exceed 1023, and subsequent
volume reads reject cylinders >=1024. An EDD read failure never changes
to CHS. MBR errors are H (header), D (disk), C (CRC), on BIOS text and
bounded COM1 output. Disk-signature bytes 440–445 are separate from code.

## Memory, firmware and options

The loader's initial stack top is 0000:7C00; pushes remain below it.
Code, boot information and buffers start at physical 0x20000 and fit
one 64 KiB real-mode segment. The BIOS scratch area 0x10000–0x1FFFF is
untouched. The MBR bounds its load against INT12 conventional memory.

Discovery records PCI BIOS, SMBIOS 2.x/3.x, EDD, APM installation and
ACPI RSDP. It never connects APM or takes ACPI ownership. SMBIOS table
reads are bounded and restricted to the first MiB; a table elsewhere
leaves manufacturer detection unavailable. Only an exact type-1
manufacturer QEMU permits fw_cfg ports. The directory walk is capped
at 256 files and requests at 64 bytes.

The existing read-only PCI input rule is included unchanged. Validated
fw_cfg platform=e500 takes precedence before A20/i8042. KBC/UART waits
are finite. Above-1MiB copies use unreal FS with interrupts disabled in
512-byte bursts; real-mode FS is restored before any BIOS call.

E820 uses at most 128 firmware calls. Invalid/truncated maps stop boot.
A 64-bit boundary sweep resolves overlaps, rounds RAM inward to pages
and coalesces equal adjacent ranges. Unknown types become reserved.
Precedence is bad memory, NVS, reserved/unknown, ACPI reclaim, RAM.
More than 128 normalized ranges is invalid. E801/AH88 are diagnostic only.

FAT access is read-only, with short names and bounded chain walks.
ELF32 is capped at 32 program headers and a 16 MiB file. All headers
and physical extents are validated before kernel writes. Segments must
fit normalized RAM in [1 MiB,16 MiB), with no physical overlap.
Dynamic/interpreter images are rejected. Virtual e_entry is translated
through the containing executable PT_LOAD.

BOOT.CFG is optional, <=127 ASCII bytes, without NUL. Default:
safe=0 serial=1. Recognized options: safe=1, serial=0, mode=0xNNNN.
Complete option bytes are copied into the NUL-terminated handoff.
Safe mode prefers 640×480; an explicitly named mode has priority.
Without a valid fw_cfg request, SELECT_READY precedes the three-second
menu: N Normal, S Safe mode, L toggle serial-log flag, P Probe.
P accepts a bounded line for up to 30 seconds:
f0:<probe|all> run=<8hex> [platform=e500].
Only fw_cfg may supply platform=e500. Unknown/malformed selectors emit
L:SELECT_ERROR and pass no request.

VBE uses VBE3 linear pitch/masks or VBE2 ordinary fields, validates each
enumerated mode, and tries the named mode then 1024×768, 800×600,
640×480, 32 then 24 bpp. 16/8 bpp are later fallbacks. Bad candidates
emit L:VBE_SKIP. Mode set requires bit 14, matching 4F03 read-back
and revalidated information; failure selects text mode 3.
EDID is read after mode selection; invalid data is cleared.

Fatal loader codes: L:A20, L:E820, L:DISK, L:FAT32, L:NO_VMM,
L:ELF, L:ELF_WINDOW, L:ELF_RAM, L:ELF_OVERLAP, L:ELF_ENTRY.
Every fatal path reports both sinks, waits for a BIOS key, then halts.

## Build and static tests

Dependencies: Python 3, NASM, ld.lld, dosfstools, mtools. T0 C/NASM
layout agreement also needs a host C compiler. From the repository root:

```sh
mkdir -p build/f0
python3 tests/loader_stub/test_static.py -v
nasm -f elf32 tests/loader_stub/stub.asm -o build/f0/stub.o
ld.lld -m elf_i386 -T tests/loader_stub/link.ld -o build/f0/VMM.ELF build/f0/stub.o
free -h
systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G -- \
  python3 scripts/build_image.py --kernel build/f0/VMM.ELF
```

Use the real kernel ELF path for integration. Options: --kernel,
--out (default build/f0/ciukios.img), --boot-cfg.
The builder replaces the image atomically after T1 checks, including
fsck.fat -n, then prints SHA-256. Scratch volumes stay beside the output
and are deleted. Backup FSInfo is synchronized after mtools updates.

## Manual focused QEMU matrix

These checks exercise the diagnostic stub, not the real kernel's F0
acceptance probes. Integrate them into the canonical runner. No rebuilds
between cases. The common lock resolves to the main checkout so all
worktrees use one inode. A failed systemd scope forbids launching QEMU.

```bash
run_loader_case() (
  set -eu
  case_name=$1; ram=$2; vga=$3; selector=$4
  image=$(realpath build/f0/ciukios.img)
  common_git=$(git rev-parse --path-format=absolute --git-common-dir)
  lock_dir=$(dirname "$common_git")/build/test-runs
  mkdir -p "$lock_dir"
  exec 9>"$lock_dir/.qemu.lock"
  flock -n 9 || { echo "QEMU lock busy"; exit 1; }
  for attempt in $(seq 1 40); do
    [ "$(pgrep -c qemu-system || true)" = 0 ] && break
    sleep 30
  done
  [ "$(pgrep -c qemu-system || true)" = 0 ] || exit 1
  awk '/MemAvailable:/ {exit ($2 < 2097152)}' /proc/meminfo
  run_dir=build/test-runs/loader-stub/$case_name
  mkdir -p "$run_dir"
  qemu-img create -f qcow2 -F raw -b "$image" "$run_dir/run.qcow2"
  if [ "$case_name" = bad-crc ]; then
    qemu-io -f qcow2 -c 'write -P 0x00 528 1' "$run_dir/run.qcow2"
  fi
  fw=()
  [ -z "$selector" ] || fw=(-fw_cfg "name=opt/it.alcybercloud.ciukios/test,string=$selector")
  scope=ciuki-loader-$case_name-$$
  trap 'systemctl --user kill --signal=SIGKILL "$scope.scope" 2>/dev/null || true' EXIT
  timeout --signal=TERM --kill-after=2s 15s \
    systemd-run --user --scope --unit="$scope" \
      -p MemoryMax=1500M -p MemorySwapMax=0 -- \
      qemu-system-i386 -accel tcg -cpu pentium3 -machine pc-i440fx-9.2 \
      -m "$ram" -vga "$vga" \
      -drive "file=$run_dir/run.qcow2,format=qcow2,if=ide" \
      -serial "file:$run_dir/serial.log" -display none -monitor none \
      -net none -no-reboot -no-shutdown "${fw[@]}" || status=$?
  [ "${status:-0}" = 0 ] || [ "${status:-0}" = 124 ] || exit "$status"
  systemctl --user kill --signal=SIGKILL "$scope.scope" 2>/dev/null || true
  [ "$(pgrep -c qemu-system || true)" = 0 ] || exit 1
  check=()
  [ "$vga" != none ] || check+=(--text)
  [ -z "$selector" ] || check+=(--request "$selector")
  [ "$case_name" != bad-crc ] || check+=(--crc-error)
  python3 tests/loader_stub/check_serial.py "$run_dir/serial.log" "${check[@]}"
  rm "$run_dir/run.qcow2"
)
run_loader_case std512 512 std ''
run_loader_case min128 128 std ''
run_loader_case text 512 none ''
run_loader_case e500 256 std 'f0:boot run=0000abcd platform=e500'
run_loader_case no-fw 512 std ''
run_loader_case bad-crc 512 std ''
```

The stub emits six raw CIUKI_TEST records (BEGIN, four DATA, END PASS),
then halts. It validates the real handoff and BSS and reports flags,
normalized-map count, input policy, framebuffer/text selection, request
bytes in hex and translated physical entry. Bad CRC must emit exactly C
and no stub records. Timeout alone is never pass evidence; the serial
checker validates the terminal records. The deliberate timeout tears down
the halted stub/error wait. The canonical runner should instead use QMP
quit after terminal evidence and keep its complete result.json.
Keep image SHA-256, profile and raw logs; remove passing overlays and
retain at most five runs per suite. No runtime qualification is claimed
here; see docs/validation/2026-10-09-f0-loader.md for actual results.
