# F1-87: T23 hold, audio, VM and IRQ-off checks

## Sources and implementation decision

Research consulted on 2026-10-21:

- Microsoft, *Windows 98 Resource Kit* (1998), chapter 10, “File Systems”
  (`RK98BOOK.CHM`, `wrkc10_7.htm`, paragraphs following Table 10.8): IFSMGR
  routes file-system calls; FSDs initiate low-level requests through IOS.
  Chapter 28, “File System Architecture” (`wrkc28_5.htm`), “Installable File
  System Manager” and “Block I/O Subsystem”: access arbitration and device
  requests have separate roles; IOS queues requests and notifies drivers.
  Read from the owner's extracted media in the main checkout; no media is
  copied or redistributed. The public [chapter 28 overview](https://library.thedatadungeon.com/msdn-2000-04/rk98book/html/wrkc28_1.content.htm)
  identifies the protected-mode file-system components.
- Microsoft DDK, [Routing Requests to the FSDs](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F6E.HTM),
  [IOS Services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM),
  [driver layers](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F5A.HTM),
  and [IFSMgr_Ring0_FileIO](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM).
  This public mirror is the Windows 95 DDK ancestry, not a claimed Windows 98
  kernel implementation listing. Request packets, handles and calldown
  ownership are separate from a file-system namespace mutex.
- [Microsoft KB Q223282](https://helparchive.huntertur.net/document/108125)
  specifically describes the Windows 98 DDK's cache-block mapping and its
  read-only, deny-write preconditions. It does not specify a general volume
  mutex release protocol.
- id Software's [Quake DOS timer](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/sys_dos.c),
  `Sys_FloatTime`: each sample latches counter 0 through port 0x43 and reads
  the low and high bytes through 0x40, alongside the BIOS tick count.
  This is the timing-poll pattern served by the fast path.
- id Software's [Wolfenstein sound manager](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/ID_SD.C)
  programs PIT channel 0 and sends ordered AdLib index/data writes, retaining
  status reads for detection/delays. The hot path must preserve the existing
  PIT latch and OPL models, not substitute physical I/O or discard writes.

These sources support separate metadata and device phases. They do not
establish a historical 20 ms lock or 33 ms VM scheduling guarantee. Those
numbers are CiukiOS acceptance limits. No NT NTVDM behavior is used here.

`VFS_IO` now drops both namespace and physical volume mutexes before FAT
work. The logical mutation guard still protects shared FAT publication and
borrowed names. Open-file readers use their own FAT workspace, retained
identity and per-description serialization; unrelated log writes, commits,
mkdir and read-only metadata requests admit those readers. Native read
syscalls enter without first taking a volume ticket. Existing callers that
borrowed names under a reservation retain that reservation contract.

**The logical guard is not a bounded metadata slice.** `vfs_hold ms=` still
measures its complete lifetime, deliberately preserving the meaning of the
F1-86 record. The host replay reports physical mutex time separately and
exposes the remaining logical-guard maximum. A passing physical mutex test
must not be presented as the directive's complete B acceptance.

## Records

`vfs_hold` and `vfs_wait` add `site=`. Static site families are `native`,
`dos`, `writer` and `vfs`; `op=` identifies read, lookup, readdir, open,
stat, write, mkdir, commit and the other existing mutations. `metadata`
identifies a generic bookkeeping turn; no `other` tag is produced. Existing
8-record/drive/2-second suppression and unlocked publication remain.

`[cpu] irqoff` adds `task=<name>#<id> worker_site=<address>`. These are copied
when the **last unclassified over-budget section ends**. They need not be
the context of the separately reported raw maximum. Zero worker_site means
no tagged device callback in that ending task. Deferred work stores its
callback address on its task, so preemption into another task does not
borrow its attribution. USB host/HID tasks identify `usb_step` and
`usb_hid_service`. Resolve addresses against the matching kernel ELF.

`[vm-audio] cadence` fields `descheduled`, `parked`, and `if_clear` are
accumulated **pending IRQ-entry milliseconds**, not scheduling counts:

- `parked`: an SB16 IRQ awaits entry while fullscreen hold, suspension, or
  park is active. This is unrelated to the client sleeping in WAIT_PRESENT.
- `descheduled`: an IRQ awaits entry and the sampled host producer context
  was another task, without the fullscreen condition above.
- `if_clear`: an IRQ awaits entry while the guest virtual IF blocks it and
  neither of the preceding conditions applies.

WAIT_PRESENT sleeps the client for at most 33 ms. A delayed presenter
coalesces dirty state; it does not gate the emulation or SB16 clock. The
window VM retains the client's normal base between waits and receives input
boosts; existing WAIT donation carries transient priorities. Fullscreen uses
the VM's original base, restored at fullscreen transitions/client entry.

## Two-boot USB attribution experiment (lead, T23)

1. Use the same canonical image/source and matching ELF for both boots.
   Keep workload, devices, display mode, elapsed session length and logging
   settings equal. Record image SHA-256 and both serial/disk logs.
2. Boot normally with the usual BOOT.CFG. Record per-controller scan rates,
   unclassified counts, elapsed time and the ending task/worker sites.
3. Boot with the exact token `usbscan=0` in BOOT.CFG (127-byte loader limit).
   Spaces and CR/LF delimit tokens. Initial root/hub enumeration still
   runs. Subsequent periodic topology scans stop; HID service, transfer
   completion and storage service remain enabled. Do not change cabling
   during this diagnostic boot: topology hotplug detection is suspended.
4. Compare unclassified sections per second, their raw/work maxima and
   ending worker sites, alongside audio cadence and CPU service. A reduction
   is correlation requiring follow-up; absence of an SMI counter on this
   CPU does not establish a firmware cause. Reboot without the token after
   the experiment; the default 250 ms scan interval is unchanged.

Host results and outstanding acceptance items are recorded in
`build/codex/f1-87-report.md`. No QEMU or hardware evidence is claimed here.

## Amendment 1: transaction prerequisite and continuous C replay

Research rechecked on 2026-10-21 against the owner's extracted Resource Kit:
chapter 10, `wrkc10_7.htm`, the paragraphs after Table 10.8, describes VFAT
lazy writes through VCACHE and FSD requests passed to IOS. Chapter 28,
`wrkc28_5.htm`, “Installable File System Manager”, “VFAT File System” and
“Block I/O Subsystem”, separates arbitration, reentrant VFAT and IOS request
queues. The public mirror URLs for those chapter fragments were unavailable
to the browsing tool; the owner's extracted chapters were read directly.
The DDK [FSD routing](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F6E.HTM),
[IOS services](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S2F60.HTM) and
[Ring0 file API](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S312A.HTM)
were also rechecked online. They document request and handle interfaces,
without specifying CiukiOS's transaction algorithm or numerical hold limit.

The binding local rules are C2 and C3 of
`docs/design/win98-class-resource-management.md` (the actual filename of the
contract named in the directive). C2 retains ordered durable stages and
finite barriers; C3 separates metadata exclusion from device completion.
The reserve/transfer/publish amendment is the lead's implementation design.

**B remains unimplemented and fails acceptance.** The cold production-stack
replay still finds a 999 ms logical `mkdir` hold, including nine 111 ms
device commands; `log_write` retains its guard through a 111 ms data command.
`test_t23_holds --require-bounded` makes this an explicit failing gate.
The normal filesystem runner retains the diagnostic replay so its other
integrity groups can run; its success does not certify B.

The publication prerequisite is absent: `cache_write()` can fetch/evict,
throttle or await an active device barrier, even on an already cached sector.
`cache_group_begin()` can also wait for that barrier. Each cache block stores
one mutable version; the dependency frontier lives on the device, not on
each staged metadata version. FAT writes a detached chain, barriers it,
links it, barriers again, then publishes the directory. The detached EOC and
the later link can occupy the same sector. Putting all final metadata into
the cache in one guard section cannot preserve those intermediate versions
with the current API. This does not rule out the lead's design: it requires
a nonblocking, reserved multi-block publication API with explicit ordered
versions before replacing FAT's barriers. No guard release or barrier
removal workaround has been applied. Reservation isolation, abort, orphan
recovery and rename/unlink-race tests for the proposed transactions remain
outstanding, along with the transaction implementation.

C now has a continuous host replay in `tests/host/t23_audio_replay_test.c`,
registered in `test_kernel_latency.py`. It runs the real scheduler, CPU
sampler/reporting, ordered timer bodies, block request engine, native mixer
and refill service. CPU/stack switching, transport registers, compressed
decoder output and the controller's sleeping completion are modeled.
It does not replace the separate production VFS B replay.

`tests/host/t23_trial.h` reconstructs 513 desktop holds (406/91/10/6 in the
measured bins, maximum 1502 ms), 35 log writes (mean 1633 ms, maximum
1837 ms), six 602 ms commits and one 441 ms mkdir over 400 seconds.
Bin interiors, arrival ordering, commit count and CPU costs are explicit
fixture assumptions; the trial does not supply individual timestamps.
Desktop operations run serially; log writes and commits share one serial
writer. Pending arrivals backlog behind the same actor. Operations issue
one command at a time, with a 111 ms service ceiling;
decoder demand uses the production urgent-reader queue context, ahead of
fresh normal desktop reads while preserving age promotion. The reservoir
starts at two seconds once, feeds a finite native stream and is replenished
only on decoder-read completion. It is never reset between holds.
The modeled producer requests more encoded input when reservoir space is
available, allowing catch-up after a delay, as Music's producer does. The
refill/transport and native PCM accounting are production code; compressed
input expands at the explicit 128 kbit/s fixture assumption. PCM conservation
is checked independently against initial frames and completed decoder reads.
Concurrent device work costs 300 microseconds per two-millisecond sleep;
the DOS peer costs one millisecond per seven-millisecond sleep. These are
CPU-load assumptions, not measured T23 callback costs.

The replay asserts zero audio-refill CPU-overrun events, transport deadlines,
underruns and source starvation; it exercises a complete 400 seconds of
interleaved requests. An injected long audio CPU turn must produce an actual
sampler overrun, providing a negative control. Passing this boundary replay
does not establish the missing transaction lifetimes or physical T23 timing.
