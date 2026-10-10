# F1-22 crash gate sequencing correction

The image `622e1632…` run reached BEGIN, storage and absent-fixture records,
then failed with `timed out` before crash ARM. The runner installed its
`pwritev` breakpoint before CPU start. `probe_mount_crash()` calls
`storage_enable_write()` before enabling the cut trace and emitting ARM;
`storage_enable_write()` enables FAT writes and creates the log directory.
Those setup writes therefore encounter the startup breakpoint without any
cut records. The old fake emitted ARM after one startup suspension and did
not model the pre-ARM write-enable sequence.

Sources checked before changing the runner:

- [QEMU blkdebug documentation](https://www.qemu.org/docs/master/devel/testing/blkdebug.html):
  tagged requests suspend at a breakpoint and must be explicitly resumed.
- [QEMU 11.1 HMP block commands](https://raw.githubusercontent.com/qemu/qemu/v11.1.0/block/monitor/block-hmp-cmds.c),
  `hmp_qemu_io()`: a block-node target creates a temporary backend and
  releases it after the command; an existing backend target avoids this.
- [QEMU backend lifetime](https://raw.githubusercontent.com/qemu/qemu/v11.0.0/block/block-backend.c),
  `blk_unref()`: releasing the last reference drains requests. Re-arming
  against the node while its write is suspended can therefore deadlock in
  HMP, consistent with the short QMP timeout and forced scope cleanup.

Decision: install the first breakpoint through QMP immediately when the
crash ARM record is parsed, before blockstats/status queries. Address the
persistent `ciuki-cut-drive` backend rather than the `ciuki-cut` node.
Re-arm before each resume, stop at exactly the declared completed trace
index with the subsequent write suspended, and retain overshoot/trace
validation. ARM receipt is asynchronous: any trace overshoot during gate
installation must fail, never be accepted as a shorter prefix.

The fake must let write-enable setup finish and emit ARM before accepting
the first breakpoint. Its gate batches represent records emitted before
the next write, not records emitted by suspension itself. Both host deadline
and QMP timeout failures record the pending declared action, synchronization
status, declared/completed cut index and suspension phase. Guest code and
the cut predicates are unchanged. QEMU validation remains with the lead.

Host validation (no emulator or systemd scope):

```text
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host
Ran 156 tests in 91.341s
FAILED (failures=1, skipped=5)

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/host -p test_mount_fixtures.py
Ran 16 tests in 10.894s
OK
```

All mount-fixture/crash-runner tests passed, including pre-ARM setup writes,
exact cut index, delayed ARM receipt (one/two completed writes accepted,
three rejected), reboot/checker comparison and four pending timeout phases.
The sole full-suite failure is the existing
`test_kernel_map_f2probes_within_rodata`: the built kernel's linked F2
registration table has eight entries, lacks `app-gate` and differs in order
from the nine entries expected by the current source/tests. No kernel rebuild
or unrelated test adjustment was performed. Semble was attempted first but
its parser download failed because network name resolution was unavailable;
discovery then used exact symbol searches and small file ranges.
