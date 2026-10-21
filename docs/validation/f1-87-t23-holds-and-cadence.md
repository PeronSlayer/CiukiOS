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
