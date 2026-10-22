# Display Properties: completion waits and system thumbnails

Research and implementation record for `f2-88c`; host evidence is recorded in
`build/codex/f2-88c-report.md`. Hardware acceptance belongs to the lead.

The Open Group [poll specification](https://pubs.opengroup.org/onlinepubs/9799919799/functions/poll.html)
defines readiness waits, including blocking until an event or a deadline.
CiukiOS has `CIUKI_SYS_POLL` and pollable duplex channels; `pipe()` is excluded.
Use a channel endpoint in the Display UI wait set for worker completion.
Publish the result with release/acquire atomics, then send a completion token;
consume it once. Do not use a periodic completion timer or a trylock loop.

Repository findings: `du_run()` sleeps every 1–10 ms; the old preview repeats a
50 ms timer; `pthread_mutex_unlock()` always calls `WAKE_WORD`; the decoder
reallocates its input geometrically and sleeps 1 ms after every read. Native
`poll()` itself calls `cu_wait_change(seq, cu_now()+1)`. Moving to application
`poll()` alone therefore cannot establish a polling-free kernel wait. This
kernel behavior and the missing native priority API were the first report's
scope questions, resolved by Amendment 1 below.

The [stb_image upstream source](https://github.com/nothings/stb/blob/master/stb_image.h)
describes memory decoding and output allocation. CiukiOS `image.c` additionally
allocates an ARGB buffer and copies the decoded RGBA pixels. A 1448×1086 scene
therefore needs two 6,290,112-byte pixel buffers, compressed input and decoder
workspace. `ua_brk()` maps an extent when the heap grows; it does not itself
issue one syscall per page. File reads pin the requested output range and
allocate a bounded kernel transfer buffer. Avoid attributing the supplied
system-time samples to demand faults without a kernel trace.

Decision: build deterministic 192×144 XRGB thumbnails of all three shipped
wallpapers using the existing production decoder/scaler. Install them beside
the wallpaper cache and inventory their sizes and hashes. A built-in path
must only load its thumbnail, including on cache failure; it must never fall
back to decoding the original. User files use the worker, one input allocation
from `fstat()`, bounded reads without artificial sleeps, and cooperative
cancellation between phases. Synchronous stb decoding cannot be interrupted
mid-call through its current interface.

Microsoft's [scheduling priorities documentation](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)
recommends lower priority for CPU-intensive background threads. Native CiukiOS
has priority mappings internally, but no native thread priority setter. A
sleep loop is not an implementation of low priority. Do not introduce one or
claim that normal-priority work satisfies this requirement.

The supplied QEMU record actually shows worker base 7 and focused UI base 9.
The production scheduler replay checks those levels and the input boost. This
provides priority relative to the focused UI, but does not implement a distinct
below-normal background class. Amendment 1 accepts these existing levels.

Host validation covers a pinned single-CPU worker/UI transport, cooperative
cancellation and failed-publication HUP, a production scheduler replay with
1 ms assumed input work and a polling negative control, three complete
thumbnail-backed dialog golden frames, the existing Display goldens and mode
transactions, payload inventory/determinism/corruption recovery, and log
deduplication. The native `poll()` kernel is not replaced by these host
transports. Amendment 1 adds native wait evidence below; physical T23 idle
CPU and input timing still require lead validation.

## Amendment 1: native event waits

The lead authorizes the native wait fix, accepts worker base 7 versus focused
UI base 9 (input boost 11), and accepts cancellation at decode boundaries.
Microsoft's [wait functions](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions)
describe blocking until an object changes or the requested timeout expires.
Its [priority guidance](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)
also requires a higher-priority thread waiting on lower-priority work to block.
Linux v6.12 [poll implementation](https://github.com/torvalds/linux/blob/v6.12/fs/select.c)
and [wait queues](https://github.com/torvalds/linux/blob/v6.12/include/linux/wait.h)
provide primary-source examples of registration, predicate recheck and signal
interruption. No upstream implementation is copied. The Open Group page still
returns HTTP 403 on direct retrieval.

Repository comparison: `net_poll()` captures a change sequence before testing
readiness, and `cu_wait_change()` plus `kwait` recheck before blocking. Preserve
both checks and the fd-generation/retained-object checks. Replace the 1 ms poll
deadline with the original absolute deadline. Treat `UINT64_MAX` as an infinite
`kwait` deadline without arming a timer; signed deadline arithmetic cannot
interpret that sentinel as a normal future timestamp. Internal lwIP semaphore
and mailbox safety deadlines retain their existing policy.

Input publication must notify the shared poll sequence, using the static
queue's deferred IRQ wake rather than walking waiters in an input IRQ. Channel
publication, consumption and peer release already notify it; fd table changes
already advance generations and notify it. Caught-signal interruption and
process stop must also advance the sequence: making a task READY alone leaves
the event predicate false and causes it to sleep again. Channel send/receive
predicates already include signal/stop and peer state, so their 1 ms safety
timeouts can become infinite event waits. This includes the idle decode worker.

Host acceptance must execute production native poll, event/kwait registration,
channel waits and scheduler/timer paths, including both pre-registration race
windows, finite/infinite waits, and each producer. Model CPU charges and input
dispatch costs remain explicit assumptions; physical T23 timing remains a lead
validation item.

The extended model executes native `net_poll()` and `sys_arch.c` without
`LWIP_HOST`, production `sync.c`, channel queues, input publication, signal
selection/interruption, fd close/generations and the scheduler/CPU sampler.
Its privileged EFLAGS read, context switching, user memory and allocator are
host boundaries. It checks three publication race windows for each producer,
real finite deadlines after spurious wakes, and ignored/masked signals.
Separate UI-poll and worker-receive cases each advance 65,000 simulated ms
without an armed wake timer or any CPU charged to the blocked task. Full-channel
send and empty-channel receive wake on publication/consumption, caught signals
and peer close. Per-tab replays use actual input-marked channel delivery and
the production priority boost. A 150 microsecond cost per wait entry and 1 ms
input dispatch/draw are explicit model assumptions. The old 1 ms wait policy
is a negative control; these costs are not hardware measurements.

## Amendment 2: merged Control Panel jobs

The f2-98a private asset, wallpaper-directory, program-inventory and font jobs
must wake the same native wait loop as the decoder. Microsoft's primary
[wait-functions reference](https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions)
describes a window thread waiting for input alongside synchronization objects
and real deadlines. Its
[update-region reference](https://learn.microsoft.com/en-us/windows/win32/gdi/invalidating-and-validating-the-update-region)
describes accumulating invalidated rectangles until painting. These contracts
support retaining f2-98a's clips while replacing its 100 Hz completion checks.
This is an application wait/update contract; no NT-specific kernel design is
inferred from the current SDK documentation.

Repository comparison: the merged `du_job_ready()` used a mutex trylock and
`du_run()` woke every 10 ms. Control Panel now uses a private one-shot job
adapter with release/acquire publication and join-before-adoption ownership.
Assets (including wallpaper enumeration), program inventory, fonts and durable
settings writes notify a shared pollable channel. The decoder's separate
completion endpoint remains in the same wait set. Notifications are bounded;
a full queue is already readable and a failed publication closes its peer to
wake HUP. A short mutex serializes shared send/close; there is no retry timer
or trylock loop. Deferred adoption during capture/modal interaction consumes
the notification and waits for the next interaction instead of spinning on a
readable descriptor.

The existing settings writer gains an optional notification callback after
result publication; other callers leave it NULL. Control Panel consumes the
writer result under its short lock because the shared desktop trylock API
requires periodic retries. No disk I/O occurs under that lock. The explicit
program-removal durable-save wait also waits on the channel instead of sleeping
in 1 ms increments.

An infinite application deadline now becomes `poll(..., -1)`. Clocks, uptime
and the external mixer snapshot retain genuine one-second deadlines; graphics
status retains its two-second query deadline when no request is outstanding;
preview debounce and display-mode confirmation retain toolkit deadlines.
Other idle pages wait indefinitely. These feature timers are not a background
job service interval. Input and completion also service pending navigation and
close without waiting for the next wall-clock second.

Page changes cancel preview state immediately, with cooperative decode-boundary
cancellation retained. Fonts remain owned across pages and are released only
on detach. A preview snapshot includes the portrait pointer, so late asset
publication retains the decoded image but requests a fresh preview rendering.
Preview completion invalidates only its 208×188 rectangle. Deferred Close
advances its window-protocol state before entering another event wait. Host
integration executes the production Control Panel loop with a virtual desktop
turn/clock/poll boundary, and real pthread jobs/channels/FreeType/settings-worker
publication separately. The native scheduler/wait model remains the production
kernel-wait proof; neither fixture establishes physical T23 timings.
