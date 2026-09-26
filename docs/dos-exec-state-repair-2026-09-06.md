# DOS EXEC state repair — 2026-09-06

The suspended parent INT 21h/AH=4Bh call used to remain included in InDOS while
its child ran. On a real installed-HDD QEMU boot, the top-level probe observed
2 after AH=34h returned, its children observed 3, and a grandchild observed 4.
The final DOSMGR caller DS/BX metadata also described the child's last DOS call
instead of the original parent's EXEC call.

The correction suspends the parent's InDOS contribution only at an actual COM
or MZ control transfer, restores it once in the return trampoline, and restores
EXEC's caller metadata from its original interrupt frame. Pre-transfer errors
and AL=03 overlay loads retain their normal dispatcher lifetime. AX/CF are
preserved while restoring metadata. Child-specific DS/ES/ZF return controls
cannot become EXEC's return controls.

A frozen before/after comparison passed in two independent copies of an
installed HDD, with 512 MiB QEMU guests and no source CD. The unpatched and
patched SYS files were 42895 and 42983 bytes respectively. These snapshots
predate the separate MZ/PSP-memory and zero-length-write fixes; this report does
not attribute Windows display/group corruption to InDOS.

The final test collected 44 records per kernel and checked 14 cases:
COM/MZ termination, COM near RET, PSP:0/INT20, AH=00h, nested COM-to-MZ EXEC,
missing file, invalid format, unsupported EXEC subfunction, allocation failure,
AL=03 overlay with byte comparison, and COM TSR. It compared preserved
registers, CF/AX, ZF, DOSMGR caller metadata and AH=4Dh exit status. The existing
CIUKPST probes independently passed PSP/DTA/vector restoration, nested COM
execution, TSR retention through another EXEC, unload and double-free rejection.

With the fix, every child and parent observation reported InDOS=0 outside DOS;
a temporary chained INT 13h probe observed InDOS=1 during actual DOS disk I/O.
Thus the test also rejects an implementation that simply leaves InDOS zero.
An additional COM launch after the TSR tests again reported zero.

Evidence: `build/full/indos-exec-repair-2026-09-06/final/result.json`, per-kernel
`serial.log`, `debug.log`, screenshots, frozen binaries and source snapshots.
The test invocation and its exact limits are documented in
`src/probes/execstate/README.md`; the reusable runner is
`scripts/qemu_test_exec_state.py`. No physical ThinkPad verification is claimed.
