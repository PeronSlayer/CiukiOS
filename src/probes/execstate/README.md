# DOS EXEC state regression

This probe executes against the real CiukiDOS kernel in an installed HDD image.
It observes the InDOS byte through INT 21h/AH=34h and the public DOSMGR caller
metadata through INT 2Fh/AX=1607h/BX=15h/CX=0. Debug-port records are emitted
before any subsequent DOS call can overwrite the observed state.

The parent also temporarily chains INT 13h, verifying that InDOS is nonzero
while DOS performs actual BIOS I/O. A byte that remains permanently zero will
fail this check. The BIOS vector is restored before the parent exits.

Run from the repository root (QEMU KVM and mtools required):

```sh
python3 scripts/qemu_test_exec_state.py \
  --image path/to/installed-hdd.img \
  --before-sys path/to/kernel-before.sys \
  --after-sys path/to/kernel-after.sys \
  --output build/full/qemu-exec-state
```

The source disk and kernels are copied, never modified. The default FAT
partition byte offset is 32256; override it with `--partition-offset`.
Both kernels must match the disk's FAT16 build geometry. The script deliberately
requires the baseline to reproduce the suspended-EXEC and caller-metadata bugs;
it is a before/after regression proof, not a general test of arbitrary kernels.

Fourteen EXEC cases cover COM and MZ termination, COM near RET, INT 20h through
PSP:0, AH=00h, a nested COM-to-MZ child, missing/invalid programs, invalid EXEC
subfunction, impossible allocation, a valid AL=03 MZ overlay with exact content
comparison, and COM TSR termination. Each EXEC records its input and output
registers, CF, ZF, caller metadata and subsequent AH=4Dh exit status.

The independent existing CIUKPST/CIUKTRM/CIUKPCOM probes additionally verify
nested COM execution, PSP/DTA/vectors, resident TSR contents surviving another
EXEC, unload and rejection of double-free. Another COM launch after those
checks verifies that InDOS remains balanced. This does not establish Windows,
video, sound or physical-device compatibility.

`result.json`, per-kernel serial/debug logs, screenshots, frozen probe sources,
binaries and kernel hashes preserve the evidence. A timeout or failed assertion
is a test failure, even if an earlier console message contained the word PASS.
