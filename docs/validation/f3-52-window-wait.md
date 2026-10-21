# F3-52 DOS window frame wait

Research checked before implementation (2026-10-20):

- [Microsoft Windows 95 SDK: priority inversion](https://techshelps.github.io/Win32Api%20Documentation/html/o9bn.3.htm)
  documents dependency-owner boosting to the waiting thread's priority. This
  is the Windows 95 dependency model, independent of NTVDM. Ciuki's R2 contract
  (`win98-class-resource-management.md`, R2) already implements this through
  owned `kwait` queues. A window waiting for its VM is a request dependency;
  use that queue rather than changing the VM's configured class. Donation is
  bounded by the donor's existing ordinary boost cap (15), propagates through
  actual downstream locks, and ends on wake, timeout, cancellation or death.
- [Linux v6.12 wait queues](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/wait.h)
  publish a waiter before checking the predicate and blocking. Reuse Ciuki's
  atomic enqueue/recheck and deadline timer, with signal/exit in the predicate.
  One binding's existing busy gate admits one waiter. The maximum requested
  timeout is 50 ms; timeout is a successful observation with no surface damage.
- [QEMU 9.2 VGA](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/display/vga.c)
  derives refresh from programmed timing and combines dirty memory with display
  state changes. [FreeVGA status registers](https://web.stanford.edu/class/cs140/projects/pintos/specs/freevga/vga/extreg.htm)
  distinguish vertical retrace from display-disabled intervals. Preserve the
  current synthetic timing and F3-41 presentation cap; observe dirty/model
  state at safe VM/deferred worker boundaries, never run conversion in an IRQ
  or a wait predicate. Keep row yields and exact damage comparison.

Repository comparison and decisions:

`dosvm_create` creates a `P_NORMAL` worker (base 7). Native POSIX threads start at base 7 too; `task_focus` adds 2 to
the focused `P_NORMAL` thread (base 9). Delivered input and completion may
raise its effective priority. F3-41's DOS preference cannot override a higher effective
priority. The frame wait donates that effective priority only while blocked.
It leaves device budgets, focused DOS budgets, quanta and cpustat unchanged.

Contrary to the initial evidence's surface-pin attribution, `surface_write`
writes already resident, retained `MM_USER_SHARED` pages without a user pin.
Their 300 pages (640 x 480 x 4 / 4096) already exist for the surface lifetime.
The explicit `ua_pin` in `dosvm_client_syscall` pins the 136-byte command.
Use bounded `ua_read`/`ua_write` for wait/present records: their transaction
holds protect the copy and resolve only the record's one or two pages, with
no `ua_pin` residency walk. Retain the surface as before, write only damaged
rows, and add no surface commitment or lifetime pin. Other operations retain
the existing record reservation. Host pin counts and test results are in
`build/codex/f3-52-report.md`.

WAIT_PRESENT extends the version-1, 136-byte ABI using the operation-specific
union, following F3-13/F3-20. It observes events without drawing over a loading
or settings panel. The next PRESENT_BANDS consumes damage only when the client
is displaying guest video. Video observations are frame paced even when the
client intentionally suppresses presentation. State and console events bypass
that cadence. Timeout lets the client service desktop messages within 50 ms.

Final review also rechecked the F3-41 sources:
[QEMU 10.1 VGA row rendering](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c)
and [Linux v6.12 cooperative scheduling](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/core.c).
The change retains live scanout across four-row safe points. DOOM's
[35 Hz game tic definition](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/doomdef.h)
remains independent of programmed VGA refresh and the window's presentation
sampling. No guest timer or global scheduling policy changes.
