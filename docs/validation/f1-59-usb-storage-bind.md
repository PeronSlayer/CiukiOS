# F1-59: bounded USB storage binding and probe completion

## Research and implementation decision (2026-10-14)

Checked before implementation:

- [USB-IF BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 5.3 and 6: data STALL requires halt clear followed by CSW; CSW
  STALL permits one halt-clear retry; reset recovery is class reset, IN halt
  clear, OUT halt clear. NAK is flow control, not success or a new deadline.
- [Linux USB storage transport](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/storage/transport.c):
  completion waits sleep, have cancellation/timeouts, and retire DMA before
  buffers can be reused. Ciuki keeps its pinned bounce buffer and retirement.
- [Intel UHCI 1.1, mirrored original](https://netwinder.oregonstate.edu/pub/misc/docs/29765002-usb-uhci%20design%20guide.pdf),
  3.2.3/3.2.4/3.4: TD MaxLen describes the transaction, the buffer must cover
  MaxLen, and horizontal QH links resume after a breadth-first TD. Ciuki's
  36/8/13-byte IN TDs target the 256-byte bounce region, never caller stacks;
  the copy is length checked after retirement. Endpoint MPS=64 does not make
  those TDs request 64 bytes. The walker is bounded and has no reclamation loop.
- [Linux mutex rules](https://docs.kernel.org/locking/mutex-design.html):
  locks must have an owner and cannot be recursively acquired. Ciuki's
  address-order checker covers `kmutex`, not `fs_lock`; switching USB to
  `kmutex` would introduce address-order failures without solving I/O waits.
- [QMP input-send-event](https://www.qemu.org/docs/master/interop/qemu-qmp-ref.html#command-input-send-event)
  injects events, not consumption acknowledgements. Runner-04 waits for fresh
  gate counters. A completed shell (stage 6, four key edges) can precede the
  script's final motion and button pair. The probe must wait for those consumed
  events; the runner's terminal refusal stays intact.

The supplied T23 evidence proves successful descriptor enumeration and a
completed GET MAX LUN, followed by loss of system progress. A partial disk
batch cannot prove the exact executing instruction or even that INQUIRY was
issued. No CPU-spin, IF-off DMA wait, unchecked stack DMA, or lock cycle has
been demonstrated by that log. Code does show two defects to remove: attach
can use two 10-second INQUIRY waits plus independent recovery waits; mounting
performs USB I/O under the shared VFS/cache locks, stalling desktop media
queries and the disk writer. Reservation waits must also be included in the
budget. These can explain apparent unresponsiveness, but do not prove a
hardware-wide hang. Stage records must reach durable disk before transfer.

Decision: one five-second budget from MAX LUN through mount publication,
short attach bulk steps within that budget, sleeping reservation acquisition,
and no shared storage mutex during unpublished mount I/O. Prepare FAT using
direct, complete block requests before publication (no second cache); reserve
scan workspace with a short mutex, then release it across I/O. After preparation
the volume uses the unchanged shared cache. Namespace mutation alone holds
VFS. Stage logging releases the USB reservation before synchronous log drain
and commit, avoiding VFS -> USB reservation -> log/VFS inversion.

## Audit and resulting behavior

`usb_storage.c`: bulk advances only after a retired ACK; a short packet/ZLP
ends the loop. CBW/data/CSW have separate deadlines, now capped to 1 s during
bind and to the one 5 s aggregate deadline. Initial INQUIRY has at most one
reset-qualified retry, readiness at most three commands, and CSW STALL at most
one halt-clear retry. Recovery and sense use the same aggregate deadline.
100 ms is reserved for TD retirement and the terminal log commit. Published
runtime I/O retains its existing transport budget and VFS serialization.

`usb_core.c`: control/descriptor/register/retirement loops are finite and
sleep between pending polls. Generic reservation acquisition sleeps; attach
and unpublished mount additionally use deadline/count-bounded acquisition.
`usb_sleep` drops the DMA mutex before sleeping and services HID after
reacquiring it. Input has a separate worker and private TDs. A failed bind
marks the device and block view quarantined; existing root/hub scans block
that port. A timeout cannot drop a reservation belonging to another client.

`uhci.c`: the general poll has a finite poll count plus deadline, and unlink
is followed by bounded frame retirement. The diagnostic QH walker has a
fixed link count and validates addresses. UHCI token length is the requested
length, OHCI BufferEnd is likewise bounded, and both DMA only to the pinned
bounce area. Neither the 36-byte INQUIRY nor 8-byte capacity stack buffer is
a DMA target. No transfer wait disables interrupts; native config accesses
briefly mask interrupts for CF8/CFC only. These paths were audited, not
rewritten as speculative DMA fixes.

Mount preparation streams complete block requests outside VFS/cache locks
and uses a reserved scan workspace with its mutex released. It creates no
private mounted cache and changes no frozen block-device or native ABI.
All partitions publish together under one short VFS critical section;
refusal leaves no partial namespace. Durability qualification, dirty-media
refusal, sense handling and the prohibition on resending writes remain.
Streaming preparation may exceed the budget on a large/slow healthy FAT;
that is an explicit `stage=mount` quarantine, rather than an extended bind.

Stage lines (`max_lun`, `inquiry`, `ready`, `capacity`, `flush`, `mode`, `mount`)
are ordinary klog records, with the INQUIRY retry flushed too. Before each
step the worker releases its general USB reservation, drains a bounded disk
log watermark, commits, then reacquires within the deadline. Terminal fault
records commit after the reservation is released. Writer/VFS contention
sleeps and can cancel. A read-only/inactive log remains inactive; a failed
boot disk cannot be made durable by the USB driver. Existing synchronous
boot-disk I/O and scheduler time are dependencies of the elapsed-time bound.

The desktop reports shell identity/stage to the probe. The shell completion
predicate requires stage 6, four key edges, and all twelve consumed button
edges (six press/release pairs, including the trailing click). The original
input counters and runner expectations are unchanged. The predicate avoids
a time delay and also avoids taking a late baseline from a coalesced report.

## Host verification

All suites and compiler invocations ran sequentially at `nice -n 19`:

- `tests/host/usb/run.sh`: PASS, ASan/UBSan; both controller engines, native
  lifecycle, merged input/grants and concurrent HID/bulk latency.
- `tests/host/usb/storage_run.sh`: PASS, ASan/UBSan; FAT12/16/32, partition
  views, no write retry, durability, removal, attach recovery, and the new
  production BOT/UHCI/FAT/VFS/bootlog regression.
- `test_runner_usb.py`: 16 tests, OK. Both declared USB scripts reproduce
  the old early-END refusal at the final A edge; the production predicate
  holds completion until the final release, and the full script passes.
- `scripts/test/host_kernel_tests.sh`: PASS with `ASAN_OPTIONS=detect_leaks=0`.
  Includes native report validation/completion, cancellable log sync,
  native/legacy namespace reattachment, and USB/storage suites. LSan requires
  ptrace unavailable in this sandbox; ASan/UBSan stayed enabled.
- `scripts/test/host_fs_tests.sh`: PASS, 16 groups, three fsck checks and
  three independent mtools comparisons. Includes 31 i686 translation units
  with exact kernel flags/frame guards. Final USB-storage and desktop-probe
  objects were also compiled with those flags after the final edits.
- Four focused desktop tests: launcher, shell, UHCI/OHCI shell replays and
  reordered firmware input: OK. USB also reaches stage 6 with slow program
  startup; all twelve button edges and all four keys are consumed.
- `git diff --check`: PASS.

The hostile bind replay uses the unchanged production run-queue selection
functions, with the USB worker blocked at each sleep and P_DEVICE writer/peer
plus P_NORMAL desktop clients dispatched. The actual disk logger writes and
commits to a separate fixed-disk view, and records are reopened through VFS.
It verifies that host, cache and scan-workspace mutexes are free at waits.

| Fault | Elapsed model ms | Result |
| --- | ---: | --- |
| Bulk IN NAK forever | 2055 | inquiry/data quarantine |
| CSW NAK forever | 2061 | inquiry/csw quarantine |
| Repeated INQUIRY STALL | 75 | inquiry quarantine |
| Capacity IN NAK forever | 1047 | capacity quarantine |
| Mount read NAK forever | 1077 | mount quarantine |
| Slow steps exhaust aggregate deadline | 4902 | mount quarantine |
| VFS client holds lock for 30 ms | 2085 | inquiry quarantine, client progresses |
| FAT scan exceeds deadline | 4902 | mount quarantine, workspace released |
| Another reservation owner for 30 ms | 2085 | inquiry quarantine, ownership preserved |
| Another reservation owner never releases | 4900 | max_lun quarantine, ownership preserved |

Every fault retains desktop/writer/peer progress and a ready controller; the
quarantine record survives disk reopen. A separate small FAT12 stick mounts
as E: with real UHCI scheduling, then safely ejects (2514 ms for the complete
bind/eject model round trip). Hardware stack switching, physical PS/2 delivery,
UART timing and real-disk flush latency are not simulated by the queue replay.

## Lead qualification still required

Build one canonical image under the lead's resource caps. Record its SHA-256
and retain earlier gates. Run `f1-usb` for UHCI and OHCI plus the PS/2 desktop
shell cases; END must follow all stimuli/consumption barriers, with stage 6,
four keys and twelve button edges. On the T23, boot with receiver and Lexar
already inserted, leaving the CD AutoPlay case present. Require either the
volume in Files or a durable quarantine naming the outer stage and transfer
substage. Continue PS/2 typing, USB input, CD/Files use and heartbeat collection
after failure; there must be no repeated bind/reset loop. Measure real bind
elapsed time and inspect complete stage records in the disk log. This worktree
ran no QEMU, hardware test, image build, commit, push or diary update.
