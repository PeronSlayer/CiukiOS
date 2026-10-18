# F1-80: storage waits and full-screen return

Research before implementation (2026-10-18), checked against `fs_port.h`,
`sync.c`, `vfs.c`, `display.c`, `dosvm_fullscreen.c`, `vga_hardware.c` and
`supersavage_mode.c`:

- [Linux v6.12 wait queues](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/wait.h)
  enqueue before checking and sleeping, and recheck after wakeup. Ciuki's
  `kwait_wait_until` already follows this protocol; `schedule` accepts a
  blocked task even with preemption holds. A hold suppresses cooperative
  preemption, not explicit sleep. Test the production path rather than the
  pthread portability implementation before changing ticket/admission locks.
- [Linux mutex rules](https://docs.kernel.org/locking/mutex-design.html)
  and [sleep/delay guidance](https://docs.kernel.org/timers/delay_sleep_functions.html)
  distinguish sleeping task context from atomic/IRQ context. Retain the
  thread-context assertion and FIFO tickets; no polling fallback or forced
  unlock of a blocked storage owner.
- [Linux filesystem locking](https://docs.kernel.org/filesystems/locking.html)
  informs the retained namespace/volume separation. No FAT algorithm, cache
  transport, device ownership or durability barrier changes are needed.
- [X.Org SavageSave/SavageWriteMode](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  preserves the standard VGA state and restores clocks/panel/streams in an
  ordered transaction. Retain that order and the adapter lease, introducing
  cooperative service only between complete indexed-register operations and
  bounded plane/palette slices.

The directive contains repository authorities, but no external source URLs.
The sources above are those cited by the affected synchronization/storage and
SuperSavage code/contracts.

The reported `fs_read` symbol is **not a filesystem read**: it is the volatile
VGA-aperture byte callback in `dosvm_fullscreen.c`. `native_read` is the
SuperSavage equivalent. Four 64 KiB planes were transferred without a single
scheduler checkpoint. This is a separate CPU starvation path from a storage
lock wait. Synchronous return checkpoints could each wait 2000 ms in
`bootlog_sync`, followed by another synchronous raw-device checkpoint.

Decision: preserve the F1-79 locks and F1-78 cache; reproduce their actual
sleeping behavior. Slice VGA transport, reject physical accesses from a VM
continuation after revocation, queue return-path durability watermarks for
the existing storage worker, and reserve recovery time by limiting the VM
park handshake. A failed handshake must still revoke the guest and restore
the desktop, leaving the VM suspended rather than killing a possible storage
lock owner. Queued logging is not evidence of an already durable checkpoint;
entry retains its synchronous durability gate.

## Measured host replay

The production `kwait`, scheduler, CPU sampler, FIFO lock/event and VFS
admission code run with modeled stack switches, clocks and device completions.
All three waits retain an additional preemption hold across the sleep.
Three device completions take 25 ms each before a quarantine error; the
independent volume completes 40/40/39 reads. Lock/event/admission waiters each
consume **0 ms CPU**, with **1 ms maximum stretch for every participating
task**, finishing after 79/79/78 ms. This rejects the proposed lock-spin
diagnosis for these contexts without substituting pthread waits.

| Replay | Before / negative control | After |
| --- | --- | --- |
| VGA capture, 262144 bytes | 524 ms maximum stretch; no peer service | 1 ms; 52 peer dispatches |
| VGA load, 262153 byte accesses including latch replay | 524 ms; no peer service | 1 ms; 52 peer dispatches |
| Real BOOT.LOG checkpoint, storage not draining | 2000 ms, `-EAGAIN` | Four return checkpoints in 0 ms, queued; one eventual commit |
| VM never parks | Old 2000 ms handshake returns `-EBUSY`, display still leased | 410 ms, desktop restored, VM suspended |

The aperture model charges 2 us per byte, not a new physical measurement.
The negative control uses the production preemption hold to suppress transport
safe points; the real wait/log policies are unmodified. With the real VM
return state machine and a modeled 160 ms native restore, the complete
`watchdog-return` → `desktop-restored` interval is **684 ms** when already
parked, **784 ms** with a 100 ms park delay, and **410 ms** when never parked
(250 ms handshake, capture skipped). A post-revocation continuation is denied
physical access even after another owner acquires the transport.

The display model separately verifies busy/failed/inactive logging, one
failure record per return, no synchronous raw/FAT I/O and reestablishment of
the synchronous durability gate on the next entry. The actual BOOT.LOG model
verifies that the queued watermark receives a later durable commit.

Executed commands and suite results are recorded in
`build/codex/f1-80-report.md`. These measurements do not establish a physical
firmware fallback bound or visible panel output. Hardware/image qualification
remains with the lead; necessary recovery is never aborted merely to hide an
overrun.
