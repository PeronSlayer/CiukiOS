# Several DOS VMs at once: M1, M2 and part of M3 — 28/29 September 2026

Roadmap phase 2 ([design](../../design-multi-vm-2026-09-28.md)). One boot of the
main image under `scripts/qemu_test_vmm.py` (QEMU with KVM, 128 MB), with the
image's own `JEMM386.EXE`, `JLOAD.EXE` and `CVSESS.DLL` and the probes
`VMMTEST.COM`, `VMMCHILD.COM`, `VMMIO.COM` (`src/probes/vm/`). Nothing here
qualifies physical hardware.

```
W=build/full/obj/vm-window; J=$W/jemm/$(cat $W/jemm/CURRENT)
python3 scripts/qemu_test_vmm.py --jemm $J/JEMM386.EXE --jload $J/JLOAD.EXE \
    --module $W/session/CVSESSION.DLL --probes <dir with the probes and VMFORK.COM> \
    --output build/tests/vm-mvm/run23
```

## What is checked

1. **Pre-emptive switching (M1).**
   - `VMMTEST` (system VM) forks VM 1 through `\VM\VMFORK.COM`; VM 1 runs
     `VMMCHILD`.
   - Both print on COM1 in loops that never yield; the lines alternate every
     two timer ticks.
   - The child's exit code (42) comes back through `VMM_STATE`.
2. **Blocked inside DOS on input (M1).**
   - `VMMTEST` waits for a key with `INT 21h AH=08h`, so InDOS is set and it
     waits in HLT inside the BIOS.
   - VM 1 kept running: it printed 14 lines and ended.
   - The key sent afterwards was read by the system VM.
3. **File I/O from two VMs (M2), in the same boot (the fork reuses the freed slot).**
   - `VMMIO` writes `\IOTEST.A` in the system VM while `VMMIO B` writes
     `\IOTEST.B` in VM 1: 40 records of 1000 bytes each, one write per timer
     tick. The writes alternated 41 times.
   - Each VM read its file back intact.
   - From the host: both files match the pattern byte for byte. Their clusters
     alternate A/B (14177, 14179, … and 14178, 14180, …), so both VMs
     allocated from the one FAT. `fsck.fat -n` finds the volume clean.
4. **A DOS window session in a second VM (M3, partial), in the same boot.**
   - `VMWTEST` (system VM) forks VM 1 running `VMWCHILD`. VMWCHILD begins its
     own CVSESSION session and device-model keyboard, sets text mode through
     the virtual VGA BIOS and writes `VM1 SCREEN`.
   - The system VM reads that text from VM 1's virtual screen (READBACK)
     while it keeps the physical display.
   - The focus moves to VM 1: the harness types `abc`, which reaches VM 1
     only. The focus comes back: `x` is read by the system VM, not by VM 1.
   - The system VM sends Esc to VM 1 through its device model (DEV_KEY). VM 1
     ends its session and exits with 0.
   - VM 1's keys were exactly `a`, `b`, `c`, Esc. Its text never reached the
     physical screen.
5. The DOS console answers after each part.

Result of the 28 September run, parts 1-3 (`vmm.json`, artifacts below):
`passed: true`. `vmm-serial.log` is the COM1 log and `vmm-after.png` the
final screen. Part 4 (M3) was added on 29 September. Its evidence and the
artifacts it ran with are in [the M3 record](../2026-09-29-boot-and-m3/README.md).

## Artifacts

| File | SHA-256 |
| --- | --- |
| `ciukios-full.img` (before the probes were copied in) | `8cbd2b057926342fa07a1cd4daa34b12de77e482e64f7bfb7f2ba19eb3918804` |
| `CVSESS.DLL` | `99122776d4e38dde46cb6387d893cecade3d75b93fc288754e689f3a10fafb38` |
| `VMFORK.COM` | `08ca5063aad6cfb3300e86d6c50b942a100b2c55af6c9c3fb1cf536e89f23170` |
| `VMMTEST.COM` | `58b51e1c608e08175991898e9cbb1cbf245d6b464bd963d7eb96826450d1df4f` |
| `VMMCHILD.COM` | `e7a1d682199d82a23ab6c9ebca864c44fe9384a574d4391bc4a530d08aa20b11` |
| `VMMIO.COM` | `b1cfa1a7cb8f11f3c78cf06ec4d6f682526030ca0694c19f80162021a766d4e8` |

The M1-M3 gate also runs as `multi-vm` in the complete profile
(`scripts/test_vm_window_profile.sh`). It passed in
`build/tests/vm-window-profile-2026-09-28f`, `...28g` and `...29a` (see
[the M3 record](../2026-09-29-boot-and-m3/README.md)).

## Not covered yet

- The desktop does not create VMs or paint their sessions yet (M4). DPMI
  programs in a second VM (M5) are untested.
- The M1/M2 key-wait part sends its key after VM 1 has ended. The focus
  routing itself is covered by the M3 part.
- An open file shared between two VMs has no SHARE-style locking, as in DOS
  without SHARE.
