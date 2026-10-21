# F3-52 DOS window frame wait

Research checked before the WIP implementation (2026-10-20), rechecked while
completing it on 2026-10-21:

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
Amendment 1 replaces the supervisor `ua_read`/`ua_write` copies with bounded
`ua_user_read`/`ua_user_write` transfers. A mutation reservation precedes the
complete range permission check; per-page resolution also enforces user
permissions. It covers only the copy and resolves the record's one or two
pages, with no separate `ua_pin` residency walk. Retain the surface as before, write only damaged
rows, and add no surface commitment or lifetime pin. Other operations retain
the existing record reservation. Host pin counts and test results are in
`build/codex/f3-52-report.md`.

WAIT_PRESENT extends the version-1, 136-byte ABI using the operation-specific
union, following F3-13/F3-20. It observes events without drawing over a loading
or settings panel. The next PRESENT_BANDS consumes damage only when the client
is displaying guest video. Video observations are frame paced even when the
client intentionally suppresses presentation. State and console events bypass
that cadence. Timeout lets the client service desktop messages within 50 ms.
Retain pending VIDEO until a presenter sample actually runs: an early present
deferred by F3-41's cadence must not lose the guest's final store. A scalar
sample counter distinguishes deferral from an unchanged completed sample,
including VBE. No extra pixel conversion or per-row clock read is added.

Wait predicates capture the caller's `proc_thread` before sleeping. A VM or
deferred worker notification must evaluate signals for that thread, rather
than for `g_current` at notification time. The host replay explicitly switches
to the producer context for notification and covers record invalidation during
the sleep, stop/fault, launch failure, suspension and linear VBE stores.

Application replay covers 20 launch/overlay scenarios plus TIMEOUT-only and
STATE-only variants of launch, stop/failure, timeout and fullscreen refusal.
Initial loading content
is painted once; subsequent progress updates affect only the 400x18 bar at
intervals of at least 250 ms. Settings paint only on selection/detail changes
and restore only their saved 480x280 rectangle. Startup errors and fullscreen
refusal notices paint once per change. The existing launch records and copy
remain intact.

Amendment 1 research (2026-10-21): Linux's
[user-access helpers](https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/uaccess.h)
keep checked user transfers distinct from privileged backing access. The
previous direct-map supervisor transfer could read PROT_NONE or write a
read-only record; a separate permission check before reserving the range
would race protection changes. Real production-uaddr tests now pass records
across page/extent boundaries through the DOSVM syscall, including another
thread protecting the output while WAIT sleeps. No record reservation remains
during the wait, and a failed whole-range output check changes no bytes.

WAIT, dirty sampling and VGA/VBE PRESENT retain absolute microsecond phases;
each deadline is quantized once, using the F3-41 sample policy. A completed
PRESENT updates WAIT and dirty sampling to the presenter's actual next deadline. This
avoids both repeated ceiling drift and alternating deferred frames when the
sample starts late. The host replay checks 140 successive WAIT-to-PRESENT
samples each at 14268 us (70.09 Hz) and 28536 us (35 Hz presentation policy),
with trapped and direct stores, a delayed first sample, 0..3 ms delayed
samples and both adjacent host-tick intervals (560 samples total).

The wait fake checks predicates only at enqueue, production notification, or
the timeout; timer ticks dispatch the real deferred VM worker. Tests contain
no explicit present checkpoint/notification. Negative controls that remove the
console wake or the worker checkpoints both fail functional assertions.
Console, ready, launch visibility,
stop and fullscreen transitions exercise their production entry points;
signal/exit delivery remains a scheduler transport seam. Real scheduler/sync
replays combine the blocked window's donation, focused VM, priority-9 desktop
and admitted device load. Eight ordinary focus ticks can overlap two admitted
4-ms device windows: the combined replay allows 16 ms desktop ready latency;
a donated input boost adds its existing 20 ms expiry (36 ms bound).
No scheduling policy or device budget is changed. Exact measured results,
toolchain qualification and remaining lead work are in
`build/codex/f3-52-report.md`.

Final review also rechecked the F3-41 sources:
[QEMU 10.1 VGA row rendering](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c)
and [Linux v6.12 cooperative scheduling](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/core.c).
The change retains live scanout across four-row safe points. DOOM's
[35 Hz game tic definition](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/doomdef.h)
remains independent of programmed VGA refresh and the window's presentation
sampling. No guest timer or global scheduling policy changes.

Amendment 2 host linkage (2026-10-21): rechecked the primary sources above,
especially Linux's checked user-access helpers. Keep the real production
`uaddr.c` permission model behind `dosvm_uaddr_model.c`; replacing it with
permissive stubs would remove the protection regressions. The main and audio
cost harness rules already link this model. The lead's actual failing object
is `dosvm_exit_bench`, which includes `dosvm_test.c`; the presenter benchmark
does too. Both benchmark rules now link the same model with the main
harness's function/data sections and linker garbage collection. All existing
test assertions and sanitizer settings remain. Amendment 2 commands, results
and sandbox limits are recorded in `build/codex/f3-52-report.md`.
