# integ-periph — Integrate the universal peripheral driver branches into one tree

- **Phase/model:** integration, Codex gpt-6.1-sol, reasoning xhigh. **Worktree:** `wt/integ-periph` from `main` (which already has f1-103, f1-104, f1-106, f1-109, f1-111 and integ-fix1).
- **Why:** 17 committed branches share files: credits and notices, `scripts/build_kernel.py`, the host suite lists, `src/kernel/core/devices.c`, `tests/host/devices_test.c`, `tests/host/abi_layout_test.c`, `src/kernel/include/ciuki/input.h`, `src/kernel/vm/dosvm_bios.c`/`dosvm_internal.h`, the audio backend enum and the coverage contract. Sequential merges produce chains of conflicts, so one integrated tree is built instead.

## Branches to integrate

| Area | Branches |
| --- | --- |
| Audio | `wt/f1-107`, `wt/f1-116`, `wt/f1-117`, `wt/f1-118`, `wt/f1-119`, `wt/f1-120` |
| Storage | `wt/f1-108` (with its sanitizer fix), `wt/f1-115b`, `wt/f1-115` |
| Input and USB | `wt/f1-122`, `wt/f1-104b`, `wt/f1-106b`, `wt/f1-112`, `wt/f1-113`, `wt/f1-113b`, `wt/f1-114` |
| Power | `wt/f1-121` |

For each branch use its merge base with `main` (`git merge-base main wt/<b>`) and its diff from there.

## Required

1. **Bring every branch's changes** into this worktree, editing the working tree only, with `git apply` or by hand; do not write `.git`, because the lead commits. Each driver's own files and tests come over unchanged.
2. **Unify the shared files.**
   - **Audio backend enum.** Every audio branch appended value 7. Assign unique sequential values: Maestro 7, EMU10K1 8, CMI 9, YMF 10, Trident 11, ALi M5451 12, or the branch's actual split if one branch defines two. Update every test and golden that hardcodes 7, and keep values 1–6 unchanged.
   - **Records and lists.** One Device Manager record list covering game port, COM, LPT, floppy, ATA chipsets, controllers and audio. One credits and notice list. One ABI oracle table.
   - **INT 11h equipment word** from the VM's BIOS data area, reporting COM and LPT counts consistently with f1-112 and f1-114.
   - **FAT12** (f1-115b) under the f1-115 floppy block device.
3. **Power.** f1-121's `power_source.h` lands with APM as one provider. f1-110 (ACPI events) is still running; leave its integration to a later step, but keep the interface stable.
4. **Verification.** Run `scripts/test/host_kernel_tests.sh`, `scripts/test/host_fs_tests.sh`, `tests/host/usb/run.sh`, `hid_run.sh`, `ehci_run.sh`, `tests/host/audio/run.sh`, `tests/host/net/run.sh`, the devices, COM, ABI and credits tests, and each branch's own test modules. Report exact commands and results. A LeakSanitizer exit after all assertions in this sandbox is not an assertion failure; say so.
5. **Report** anything you could not integrate, with the reason.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Host tests one at a time with `nice -n 19`. Write `build/codex/integ-periph-report.md`.
