# F1-106b: TT scheduling and HID worker deadlines

Research and implementation record; host model evidence does not qualify hardware.

- [Intel EHCI 1.0](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
  sections 3.6 and 4.12: non-HS QHs carry EPS, hub address and downstream
  port; control sets C. Async S/C masks are zero. Periodic QHs use start and
  complete masks. Use schedules contained within a frame, without FSTNs.
- [USB 2.0 original specification, IEEE archive](https://ethw-images.s3.us-east-va.perf.cloud.ovh.us/ieee/3/32/Usb_20.pdf),
  sections 11.14–11.21, 11.23.2.1 and 11.24.2.3/9: include TT think time,
  reserve the downstream bus and three complete opportunities. ClearTTBuffer
  is defined only for control/bulk; interrupt failures require ResetTT.
  Single TT requests use index 1. A TT reset does not reset device addresses.
- [Intel UHCI 1.1](https://ftp.netbsd.org/pub/NetBSD/misc/blymn/uhci11d.pdf),
  sections 3.2 and 4.1.2: IOC on a pending TD signals its completion; a
  completed IOC TD must be removed to prevent repeated frame interrupts.
  UHCI has no running root-port change interrupt: its topology scan deadline
  is distinct from HID polling.
- [Compaq/Microsoft/National OHCI 1.0a](https://www.cs.usfca.edu/~cruse/cs698s10/hcir1_0a.pdf),
  sections 4.3.1, 6.4.4–6.4.5 and 7.1.4–7.1.6: DI=0 publishes the done
  queue through HCCA and WDH. Preserve the existing done-list interlock and
  consume saved lists before reusing TDs; do not enable SOF interrupts.

The baseline keeps interrupt TDs linked across NAKs, but its native HID
worker wakes each millisecond to inspect them. Replace that deadline with
completion notifications, retirement/rearm deadlines and keyboard repeat.
Keep the existing topology policy; record its separate wake cost explicitly.
The common scan can inspect latched completions to detect a lost IRQ; only
observed completion-without-interrupt enables the short fallback.

The baseline rejects all non-HS hub children. Extend its existing QH engine,
one external hub level, pinned arena and monotonic device records. Preserve
the BOT policy and uncertain-DMA quarantine. Do not retry uncertain writes.
Hub recovery runs under the general reservation, outside interrupt context.

## Host evidence

`tests/host/usb/run.sh` and `storage_run.sh` pass. The native worker replay
with real UHCI/OHCI descriptors observes zero periodic HID wakes in an idle
second and one completion notification for both input reports; retirement
uses a deadline. Completion-to-worker service is 0 us with IRQ delivery,
at most 3500 us with the recorded 4 ms fallback. The existing latency corpus
continues to meet its 20 ms bound, including delayed dispatch and bulk NAKs.

The EHCI model passes separate SS/CS execution, masks and TT routing,
single/multi-TT conflicts, 8/32 FS-bit think times, HS budget exhaustion,
STALL/error/timeout cleanup, both control directions, failed ClearTTBuffer,
periodic ResetTT and failed reset isolation. A cancelled active pipe retains
its bus-time reservation until two HC frames prove retirement. The mixed
fixture enumerates a LS keyboard and FS mouse and services them alongside a pre-enumerated FS
bulk stick. A separate real BOT/FAT fixture verifies FS-through-TT bind,
short INQUIRY, block read/write/flush and stop. An idle EHCI input worker has
zero periodic wakes in 1000 ms; one IOC and retirement deliver its key.
Qualification tests distinguish pending ITC from a proved lost interrupt.

Both EHCI executables reach their final PASS records under ASan/UBSan, but
the unmodified LSan gate then exits 1 because ptrace is unavailable in this
sandbox. This is not a green leak-detection result. Exact commands, logs and
native compile recipe are in `build/codex/f1-106b-report.md`.

Native i686 layout: host 54540 -> 54636 bytes, device 3336 -> 3340, HID
424 -> 424, storage 1264 -> 1264. Four retained hosts grow by 384 bytes;
the DMA arena stays 20480 bytes per controller. All eight USB translation
units compile with the canonical CFLAGS and 4096-byte frame limit. ABI and
public SDK headers are unchanged.

## Qualification limits and lead decision

No QEMU, OS-image build or hardware run was performed. Physical TT timing,
chipset IRQ delivery and DMA retirement still require lead qualification.
One hub level and the existing endpoint bounds remain. Frame-contained
periodic admission may refuse a tree that a scheduler with FSTNs and frame
phase spreading could accept; no silent oversubscription is permitted.

The general worker retains four topology deadlines per second. Literal
zero total idle host wakes requires a separate policy/implementation
decision, particularly UHCI suspend/remote wake and hub change-endpoint
handling. A newly lost IRQ after qualification is discovered at that
topology deadline, then confirmed after the grace period; it cannot be
promised the working-IRQ input latency bound during initial detection.
