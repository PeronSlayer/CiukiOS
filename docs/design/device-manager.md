# F2-41: read-only device inventory and Control Panel applets

Research checked on 2026-10-12 before implementation:

- [Microsoft Device Manager](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/using-device-manager)
  presents device classes, status and driver details through device Properties.
- [Linux PCI resource attributes](https://kernel.org/doc/html/v6.7/PCI/sysfs-pci.html)
  separate read-only IDs, class, IRQ and resource addresses from operations which
  enable hardware, map registers or remove devices.
- [Upstream PCI product IDs](https://github.com/pciutils/pciids/blob/master/pci.ids)
  identify the baseline chips. A small UI naming table does not confer driver support.
- [Linux x86 CPU identification](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/cpu/common.c)
  checks supported CPUID leaves before reading extended brand strings.

Repository comparison: registry.c retains read-only boot PCI configuration,
resource leases and generations. core/init.c retains activation results and
reasons. ATA's normal lookup hides quarantined devices, so inventory needs a
separate read-only accessor. No PCI BAR extents exist for unbound devices.
fs/fs_port.c owns native CMOS calendar reads (IRQ8 stays masked); its service is represented as rtc, without
reading CMOS during inventory queries.

Decision: call 75 copies a version-1, fixed-size snapshot, capped at 64 devices,
16 built-in drivers and 64 leases. Caller supplies version and size and the SDK
supplies the buffer extent. Invalid versions/extents return EINVAL; inaccessible
ranges return EFAULT before snapshot/copy. The entire output is zeroed, contains
no pointers, and all strings terminate. Truncation is explicit. Snapshot storage
is static, following call 70: the UP kernel does not preempt kernel code, and
snapshot/copy never sleeps. If IRQ quarantine changes the registry generation
while formatting, query retries once and returns EAGAIN if it changes again.
Registry fields are copied under a short IRQ lock;
formatting and CPUID run with interrupts restored. Refresh never probes ports,
sizes BARs, changes claims, retries activation or recovers quarantine.

Resources use start inclusive/end exclusive. A PCI BAR with unknown size has
end=start and the UNKNOWN_SIZE flag; it is displayed as a base address only.
Known extents come from retained registry claims. Matching an address alone
never proves a native driver is bound. AC97 binding uses the driver's retained
BDF; VBE framebuffer and legacy ATA are separate logical devices. Unbound PCI
functions retain firmware configuration and report that policy explicitly.
Activation sequence 0 means no ledger entry (core/loader initialization).
Generation 0 means an immutable boot inventory record without a registry claim.
Lease state is exposed separately from device status. Built-in drivers use the
kernel build ID rather than invented independent version numbers.

Control Panel starts in an icon folder. Opening an applet replaces the contents
of the same window; Back returns to the folder. System and device Properties
offer General/Hardware/Advanced tabs and General/Resources views, respectively. The existing
Display, Mouse, Keyboard, Sounds and Date & Time settings retain their behavior.
Network is reserved for F6. Power explains the existing Start > Shut Down flow;
power calls remain restricted to the desktop, which owns program closure and disk flushing.
Advanced edits serial logging for the next boot only, preserving other BOOT.CFG
tokens, respecting the loader's 127-byte ASCII limit and syncing the existing
file through the native VFS. The checkbox follows ciukldr/menu.inc: serial
logging defaults on and any serial=0 token latches it off. Saving rewrites all
serial tokens consistently. This write uses the existing native pwrite,
ftruncate and fsync calls, with best-effort rollback on an I/O error; it does
not promise an atomic replacement during power loss. Read/write/sync errors
are shown to the user.

Host tests and review renders are implementation evidence; the lead must qualify
call 75 and navigation in QEMU and on the T23 before integration.
