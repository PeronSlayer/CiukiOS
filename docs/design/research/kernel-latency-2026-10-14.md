# Kernel latency on one CPU: research and proposal (2026-10-14)

Input to directive `docs/directives/f0-20-system-stalls-attribution.md`.
Evidence: T23 (Pentium III-M 1.13 GHz), image 66843edc, verbose boot,
`legacy/local/physical/66843edc/disk-log/BOOT.LOG.clean` (833 lines, 81,492
bytes). Line numbers refer to `main` at 4bc615b5.

## Summary

1. The largest stall source found in the code is not in the f0-20 candidate list: `serial_write` drains the UART **synchronously in the caller** (`serial.c:131-158`, `103-129`). COM1 runs at 38400 8N1 (`ciukldr/platform.inc:512`, divisor 3), so each byte costs 0.26 ms. A 90-byte record costs 24 ms and a 243-byte `[display]` record 63 ms, all of it non-preemptible.
2. This affects **every boot on a machine with a COM port**, not only verbose ones. Example: `[usb] worker ... scans=` (`usb_core.c:490`, not gated by verbose) is 3 records/s, about 73 ms/s of spinning (7 %) in the P_DEVICE `usb-host` task. Serial is buffered only while audio plays (`audio_dev.c:323`, `352`).
3. In a verbose boot, until the desktop takes the display grant, each scrolled line also repaints the whole 1024x768x32 console (`console.c:157-183`): 786,432 pixel stores, about 3 MiB. The T23 measured a full 3 MiB present at 29 ms (`present_us=29000`), so a line costs about 30-45 ms. Interrupts are disabled for about 0.6 ms per row (`console.c:52`), above the 250 µs budget.
4. Measured aggregate: `usb-2474` first scan at tick 6878 and `usb-2478` first scan at 9868, which is 2990 ms for about 74 records (the UHCI schedule dump, `uhci.c:53-95`). That is about 40 ms per record, all inside one non-yielding `usb_step`. The run holds 212 dump records (18.6 KB): 4.8 s of UART time alone.
5. Correction to f0-20: console painting **after** the grant is already suppressed (`console.c:54`, `162`; `grants.c:63`). The wallpaper decode and scale are user code (`apps/desktop/wallpaper_io.c:168-172`), not syscalls. Their 14.5 s is starvation, not one kernel path.
6. Second finding: a timer wake does not preempt a lower-priority running task. `sched_tick` makes the task ready but never sets `g_need_resched` (`task.c:343-347`). A P_DEVICE task waking from `task_sleep_ms` therefore waits up to the 10 ms quantum behind a ring-3 thread.
7. A P_DEVICE kernel task can monopolize the CPU indefinitely. Kernel code is never preempted (`trap.c:114`, `150`: `from_user` only), and `task_yield` from a P_DEVICE task only reaches other P_DEVICE tasks (`rq_pop`, `task.c:82-91`). The starvation boost (`task.c:350`) needs a schedule point to take effect.
8. `bootlog_sync` runs up to 8 times per USB bind (`usb_storage.c:355`, `434`, `452`, `470`, `481`) on the single `usb-host` task, which serves all 3 controllers (`usb_core.c:617-622`). In the run, the 5 s bind deadline expired (`bind_left_ms=98`, `stop=bind_deadline`) and the stick was quarantined.
9. The desktop and all native threads run at P_NORMAL (`task.c:241`), although `execution-abi.md:222-223` puts the desktop at `interactive`.
10. There is no per-task CPU accounting: `struct task` (`task.h:29-55`) has dispatch counts and the maximum ready-wait only. Nothing can name a hog.
11. Research outcome: Win9x VMM (non-reentrant ring 0, event plus boost scheduling) and Linux voluntary preemption (`cond_resched` at `might_sleep`, +0.2 % text) both met desktop latency **without** full kernel preemption. Haiku and SerenityOS preempt kernel code but need lock and critical-section discipline, which CiukiOS does not have (`execution-abi.md:226-228`).
12. Linux fixed exactly finding 1 by moving console output to printer threads: nbcon consoles in 6.12, with synchronous `write_atomic` reserved for emergencies.
13. Recommendation: keep the kernel non-preemptible, but give every ring-0 entry a bound:
    - an explicit preemption point (`kpreempt_point`) in every loop that can run longer than 1 ms;
    - P_DEVICE overrun demotion, as in Linux RT throttling;
    - no synchronous console or serial output outside panic;
    - disk I/O only on the storage task;
    - a 2 s `[cpu]` share record with the longest non-preemptible stretch and its EIP.
14. Latency budget derived from the system: audio lead is 8 x 256 frames at 48 kHz = 42.7 ms (`audio.h:10-11`). With the 10 ms quantum, any ring-0 stretch above about 10 ms risks an underrun. The HID poll interval is 8 ms. Target: ≤1 ms between preemption points, hard limit 5 ms; IRQ-off ≤250 µs (existing).
15. Ordered changes (section 4):
    - C1, serial never spins: largest gain, low risk.
    - C2, wake preemption: one line.
    - C3, accounting record.
    - C4, coalesced console repaint.
    - C5, `kpreempt_point` plus device overrun demotion.
    - C6, bind without `bootlog_sync` (with f1-62).
    - C7, one USB worker per controller.
    - C8, spawn/ELF zero and copy chunking.
    - C9, desktop UI thread at P_INTERACTIVE.
    - C10, 1 ms pollers to deadline sleeps.
    - C11, lazy CR3 for kernel threads.
16. Every change has a host test on existing harnesses: `serial_audio_test.c` (clocked 38400 UART), the extracted `sched_tick` (`test_signal_fault_records.py:270`), and the extracted `rq_push`/`rq_pop` (`tests/host/usb/storage_run.sh:25-38`, `bind_freeze_test.c`).
17. Expected T23 effect of C1+C2+C4 alone: the USB dump goes from about 3 s per controller to under 50 ms of CPU, and the steady state loses about 7 % CPU and 25-75 ms stalls each second. This remains to be confirmed by the C3 record on hardware.
18. Open measurements (no number exists yet): memset/memcpy bandwidth on the T23, and the 424 ms cached spawn→ELF of Files (1,475,164 bytes with `disk_bytes=0` is 3.5 MB/s, far below RAM speed). The C3 record must attribute both.
19. Not recommended now: full kernel preemption (lock audit of the whole kernel). Revisit only if the C3 record still shows stretches >10 ms after C1-C8.
20. No QEMU result or hardware measurement backs this document; it is a design input. Numbers marked *est.* are estimates.

## 1. Research: how others bounded kernel latency

| System | Mechanism | Primary source | What CiukiOS takes |
| --- | --- | --- | --- |
| Win 3.x/95 VMM | Ring 0 non-reentrant. A primary scheduler runs the highest execution priority, and a time slicer boosts VMs in turn. Device work runs as events "called back just before the VMM returns to the application". Boost ladder: `Cur_Run_VM_Boost` < `Low_Pri_Device_Boost` < `High_Pri_Device_Boost` < `Critical_Section_Boost` < `Time_Critical_Boost` ("events that must be processed even when another VM is in a critical section"). `Begin_Critical_Section` boosts the owner. `Call_When_Idle` runs background work only "when all VMs have released their time-slice". `Release_Time_Slice` lets idle VMs yield. | MS Programmer's Library 1.3, Windows 3.1 DDK VxD reference (sections 16.5.1 and the VMM service chapters): https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/ (the Win95 VMM keeps these services and adds threads in the System VM) | Device work as short events with a temporary boost, not as long loops at top priority. Background work (console repaint, log drain) only when idle. The focused VM is already boosted (`FOCUS_BUDGET`, `task.c:40-41`). |
| Linux voluntary preemption | `CONFIG_PREEMPT_VOLUNTARY` "adds a cond_resched() call to every might_sleep() check". "+0.2% of .text, preempt is +3.5%". It is "lighter than CONFIG_PREEMPT - at the cost of not having as tight latencies". | Ingo Molnar's patch: https://lwn.net/Articles/137259/ | `kpreempt_point()` = `cond_resched` (C5). `fs_service` (`fs/fs_port.c:116-122`) already is one, but limited to the FS. |
| Linux softirq/ksoftirqd | `__do_softirq` restarts at most `MAX_SOFTIRQ_RESTART` (10) times or for `MAX_SOFTIRQ_TIME` (2 ms), then wakes `ksoftirqd`, a normal-priority thread. | https://github.com/torvalds/linux/blob/v6.12/kernel/softirq.c | High-priority deferred work has a time cap, then continues at normal priority (C5 demotion; `kwork` today only reports overruns, `work.c:94-111`). |
| Linux RT throttling | `sched_rt_runtime_us` 950000 of `sched_rt_period_us` 1000000, so "a run-away real-time task will not lock up the machine". | https://docs.kernel.org/scheduler/sched-rt-group.html | Cap the P_DEVICE class (C5). |
| Linux printk | Console drivers are "slow but are nonetheless called with interrupts disabled". The fix uses per-console printer kthreads, with synchronous `write_atomic` only for emergencies. Merged as nbcon in 6.12 (`write_thread`, `write_atomic`, per-console kthreads). | https://lwn.net/Articles/800946/ ; merge c903327d: https://android-kvm.googlesource.com/linux/+/c903327d3295b135eb8c81ebe0b68c1837718eb8 | C1 and C4: a log-writer thread; panic keeps the synchronous path (`serial_panic`, `serial.c:164`). |
| Linux workqueues | Deferred work runs in kernel worker threads; long or blocking work uses dedicated or unbound workers. | https://docs.kernel.org/core-api/workqueue.html | Disk I/O only on `storage-writer`, never on device workers (C6). |
| Linux latency diagnosis | The ftrace `preemptoff`/`irqsoff` tracers record the maximum non-preemptible section and its site. The soft-lockup detector reports a CPU looping in kernel mode without scheduling. | https://docs.kernel.org/trace/ftrace.html ; https://docs.kernel.org/admin-guide/lockup-watchdogs.html | Stretch sampler with EIP in the timer IRQ (C3). |
| Linux lazy TLB | Kernel threads borrow the previous `active_mm` instead of loading a page directory. | https://docs.kernel.org/mm/active_mm.html | C11 (CR3 is rewritten on every switch, `task.c:325`, and PGE is off, `entry.asm:49-51`). |
| BeOS / Haiku priorities | Real-time 100-120 runs "as soon as it's ready". `B_REAL_TIME_DISPLAY_PRIORITY` 100 is for "the app_server poller and mouser" (10-30 ms). Window threads use `B_DISPLAY_PRIORITY` 15, above `B_NORMAL_PRIORITY` 10. Real-time threads must not allocate, and the file system "may lock I/O for long periods". | Be Newsletter 1-37: https://www.haiku-os.org/legacy-docs/benewsletter/Issue1-37.html | The desktop UI thread goes above its workers (C9). Device-priority tasks never touch the FS synchronously (C6: `usb-host` calls `bootlog_sync`). |
| Haiku kernel | Preemptive in kernel mode: `x86_hardware_interrupt` calls `scheduler_reschedule()` when `invoke_scheduler` is set, without a user-mode check. | https://github.com/haiku/haiku/blob/master/src/system/kernel/arch/x86/arch_int.cpp | Shows the cost: spinlock and IRQ discipline everywhere. Not adopted now. |
| SerenityOS | `Scheduler::timer_tick` also preempts kernel threads. `yield()` inside `Processor::in_critical()` returns `Delayed` and the switch happens on leaving the critical section. Time slice is 2 units of 4 ms. | https://github.com/SerenityOS/serenity/blob/master/Kernel/Tasks/Scheduler.cpp | The deferred-switch idea for `kpreempt_point` while a non-sleeping lock is held. |
| DOSBox | `normal_loop` runs the guest and polls host events (`GFX_PollAndHandleEvents`) between 1 ms ticks. The catch-up backlog is capped at 20 ms, and auto-cycles cuts `CPU_CycleMax /= 3` when it falls behind. | https://github.com/dosbox-staging/dosbox-staging/blob/main/src/dosbox.cpp | DOS VM slices stay ≤1 ms with input between them (already `dosvm.c:812`). Cap catch-up. |
| DOSEMU | `HogThreshold` releases the CPU while DOS busy-polls the keyboard. | https://dosemu.sourceforge.net/docs/README-tech/1.4/x1444.html | Already covered by the f3-17 idle detection. Nothing new. |

Decision input for `execution-abi.md:311` ("whether F2 needs kernel preemption at all, or only shorter kernel paths"): shorter paths plus explicit preemption points plus a device-class cap (Win9x and Linux-voluntary model). No full kernel preemption.

## 2. Latency model

**Budgets** (T23 class, one CPU, 1 kHz PIT, 10 ms quantum `task.c:25`):

| Consumer | Deadline | Source |
| --- | --- | --- |
| AC'97 refill | 42.7 ms of published audio (8 x 5.33 ms) | `audio.h:10-11` |
| USB HID | 8 ms interrupt interval | log `hid poll ... interval=8` |
| Desktop input→frame | 50 ms | `desktop-redraw-performance.md:112` |
| DOS VM PIT | 1 ms ticks, focus 8/10 | `task.c:40-41` |

**Rules:**

- **R1 IRQ-off.** ≤50 µs SHOULD, ≤250 µs MUST (existing, `device-firmware-ownership.md:327-333`, `timing.h:8`). Violations today: console row drawing under `line_begin` (about 0.6 ms per row, `console.c:52`), and a `klog` with IF=0 while serial is not buffered (spins with IF clear, `serial.c:107-127`).
- **R2 Ring-0 stretch.** This is the time a task runs in kernel mode without passing a scheduling point. ≤1 ms SHOULD, ≤5 ms MUST. Exceptions must be named: panic, mode set and VBE calls, and firmware calls in the BIOS VM. Every loop whose worst case exceeds 1 ms calls `kpreempt_point()` at least every 64 KiB copied, every 16 pages zeroed, every log record, every FAT cluster batch (as `fat.c:617`), and every device poll iteration.
- **R3 Device class.** A P_DEVICE task runs ≤2 ms per wake SHOULD. After 10 ms without blocking, it is demoted to P_NORMAL until it blocks, and an `[sched] device_overrun` record names it. This is Linux RT throttling and `ksoftirqd` applied to one class.
- **R4 Wake preemption.** Any wake of a task with a higher priority than `g_current` sets `g_need_resched`: the timer wake (missing today) and the kwait wake (present, `sync.c:143`).
- **R5 Output.** Logging never waits for a device outside panic. Records go to RAM rings: the disk log (exists), serial and console. Threads drain them:
  - serial: `log-writer`, P_DEVICE, ≤1 ms per wake, paced by THRE;
  - console LFB: repaint coalesced to ≤10 Hz in ≤1 ms row chunks at P_NORMAL, and never while the desktop holds the grant (exists);
  - disk: `storage-writer` (exists, `mount.c:599`).
- **R6 I/O offload.** Only the storage task and its explicit clients' syscalls wait on disk. Device workers (USB, audio, input) never call `bootlog_sync` or VFS. They post a request and continue.
- **R7 Accounting.** Every switch charges TSC time to the outgoing task. Every timer IRQ that interrupts ring 0 extends the current stretch, and a stretch above 20 ms records its EIP once (section 5).

## 3. Audit of identified paths

Cost per byte of synchronous serial = 1/3840 s = 0.26 ms. T23 full-frame LFB write = 3 MiB in 29 ms (log), so about 108 MB/s.

| # | Path | Where | Worst case | Evidence / note |
| --- | --- | --- | --- | --- |
| A1 | Synchronous UART drain for every `klog`/`rec_*` | `output.c:19-24,29-40` → `serial.c:131-158` (spin `103-129`) | len x 0.26 ms. 90 B = 24 ms; 243 B = 63 ms; `device_write` cap 1024 B = **267 ms** (`devnodes.c:29`) | Present in every boot with COM1 (`boot_flags=00000a0a`, bit 1 `SERIAL_LOG`). The 81,492 B in this run equal 21 s of UART time; the serial capture holds only 38,146 B because of the buffered drops during audio. |
| A2 | Periodic `[usb] worker` record, not verbose-gated | `usb_core.c:490` | 3 x 92 B per s = about 73 ms/s (7 % CPU), one block | All boots with COM1 |
| A3 | Console scroll full repaint before the grant | `console.c:157-183` from `console_write:330` | 6144 cells x 128 px = 786,432 stores ≈ 3 MiB; ≥29 ms (LFB bandwidth) + per-pixel `put_px`/`pack` overhead ≈ **30-45 ms/line** *est.*; IF off about 0.6 ms per row | Verbose, safe and text boots only; zero after `grants.c:63` (`console.c:54,162`) |
| A4 | UHCI schedule dump, one loop | `uhci.c:53-95`, called `usb_core.c:564` | about 72 records x (A1 + A3) ≈ **3 s** per controller | Measured 2990 ms between first scans (ticks 6878→9868) |
| A5 | Savage init trace | `savage2d.c:509-519` | 26 records, `trace_us=1617447` (wall time, with yields) | Verbose |
| A6 | HID pipe diagnostics | `usb_hid.c:83` | 32 records x 141 B → 37 ms each | Verbose |
| A7 | `bootlog_sync` in USB bind | `bootlog.c:135-174`; `usb_storage.c:355,434,452,470,481` | Up to (queued/4096+1) 4 KiB writes per call, up to 1 MiB (`bootlog.h:8`) → 257 writes + commit; ×8 per bind. Sleeps in ATA (CPU-light) but blocks `usb-host` | 5 s deadline consumed, stick quarantined |
| A8 | One `usb-host` task for all controllers | `usb_core.c:617-622` | Sum of all controllers' steps: debounce up to 2 s (`455-467`), enumeration, bind 5 s (`usb.h:19`) | `scans=1 elapsed_ms=9164/6230/1737` |
| A9 | Timer wake does not request a reschedule | `task.c:343-347` | Wake latency up to `slice_left` = 10 ms behind ring 3 | Affects `usb-hid`, `audio-refill`, `storage-writer` (all `task_sleep_ms(1)`) |
| A10 | Device task yields only to device tasks | `task.c:82-91`, `task_yield:369` | Unbounded for P_NORMAL/INTERACTIVE while a P_DEVICE loop runs | Starvation boost `task.c:350` acts only at a schedule point |
| A11 | Eager zeroing at map | `uaddr.c:102-120` (`reserve_pages`), `pte:36-52` | 4 MiB = 1024 pages, no yield; ≈4-8 ms *est.* at 0.5-1 GB/s memset (to measure) | The f2-60 4 MiB arena and 3 MiB wallpaper buffers |
| A12 | ELF copy | `elf.c:117-149` | 2 KiB per iteration, no yield; image 402,142 B for Files (`launch-image`) | Cached spawn→elf 424 ms for 1,475,164 B (`launch-stage pid=4`): unexplained, needs C3 |
| A13 | `proc_spawn` in the caller's syscall | `spawn.c:269-360` | Snapshot + map + load, one kernel entry; waits only inside ATA/FS | Desktop `frame_max_us=3297000` during the Files launch |
| A14 | `[display]`/`[desktop]` user records via stderr | `devnodes.c:25-46` | 243 B = 63 ms per 2 s inside the desktop thread; stall records about 20 ms each | Verbose |
| A15 | 1 ms pollers | `usb_core.c:621,632`, `mount.c:590`, `audio_dev.c:430-435`, `biosvm.c:503` | About 5 kernel wakes/ms, each with 2 CR3 writes (`task.c:325`) and a full TLB flush (PGE off) | Cost not measured *est.* 1-3 % |
| A16 | `sched_tick` all-task walk with IF off | `task.c:342-359` | O(tasks) per ms; about 30 tasks → a few µs | OK |
| A17 | `kwork` budget | `work.c:92-111` | Reports >1 ms, never preempts | No `[work] budget exceeded` in this run |
| A18 | Wallpaper decode/scale | `wallpaper_io.c:168-172` | User code, preemptible; 14.5 s wall = starvation (A1-A10) + own cost | Only `read` 64 KiB chunks (`WALL_IO_CHUNK`) and `malloc`/`mmap` (A11) enter the kernel |

Reconstructed boot window (ticks): `usb-host` starts at 6847. `usb-2474` is dumped from 6878 to 9868. `usb-2478` then enumerates (keyboard and mouse, 32 HID pipe records) and dumps by 14385, while the Savage trace runs. The first periodic report comes at 16042: one scan in 9164 ms. The desktop was spawned at about 6800, and its `settings_read` lands 7032 ms after entry, inside the same window. Most of that window is A1+A3 inside one non-yielding P_DEVICE task, about 40 ms per record.

## 4. Ordered changes

| # | Change | Files | Risk | Host test |
| --- | --- | --- | --- | --- |
| C1 | Serial never spins outside panic. After `sched_start`, `serial_write` only enqueues; the ring grows from 8 to 64 KiB. Drain: `serial_tick` (exists) plus a `log-writer` P_DEVICE thread that fills the FIFO while THRE is set and sleeps 1 ms otherwise. Evidence records (`rec_emit`) in task context above 75 % fill sleep for space (back-pressure, lossless). With IF=0 or in an IRQ they enqueue or count a drop (never drain inline). `serial_panic` keeps the synchronous path. | `core/serial.c`, `core/output.c`, `proc/devnodes.c` | Low. Risk of evidence loss on QEMU bursts, covered by back-pressure (QEMU's THRE is always ready, so the thread drains at memory speed). | `tests/host/serial_audio_test.c` (clocked 38400 UART). New case: 300-record task burst, assert 0 polls inside `serial_write`, 0 loss for `rec_emit`, drop counter only for IF=0 producers. Then `tests/host/test_runner.py` evidence parsing. |
| C2 | Wake preemption: in `sched_tick`'s wake branch, `if (t->prio < g_current->prio) g_need_resched = true;` | `core/task.c:343-347` | Very low; more switches only when a higher class wakes | Extracted `sched_tick` (`test_signal_fault_records.py:270`): a device task wakes during a ring-3 slice and is dispatched on the next tick. `dosvm_idle_test.inc` focus policy stays green. |
| C3 | Accounting and stretch sampler, plus the `[cpu]`/`[kernel]` records (section 5) | `core/task.c`, `task.h`, `arch/pic_pit.c:57` (pass `tf`), new `core/cpustat.c` | Low; one `rdtsc` per switch, one compare per tick | Replay `schedule()`/`sched_tick` with scripted run lengths: per-mille sums to 1000±1, stretch >20 ms reports the injected EIP, idle excluded, no record when nothing exceeds thresholds |
| C4 | Console LFB repaint coalesced. `console_write` only appends history and sets dirty. A `console` P_NORMAL thread repaints ≤10 Hz, one row per `kpreempt_point`. IF stays enabled while drawing; the `drawing` CAS in `fbdev_console_begin` already arbitrates. Panic, `console_reclaim` and overlays stay synchronous. | `core/console.c` | Low-medium (panic path must still paint) | New host compile of `console.c` with a counting fake framebuffer: 500 lines in 100 ms give ≤2 full repaints; panic paints immediately; no draw while owned |
| C5 | `kpreempt_point()`: if `g_need_resched` and IF=1 and no `kmutex`/`fs_lock` hold that forbids it, call `schedule()`. Device overrun: a P_DEVICE task running 10 ms since dispatch without blocking is requeued at P_NORMAL until it blocks, with a record. Insert points: diagnostic dump loops (`uhci.c` loop, `savage2d.c:509`, HID diagnostics), `elf_load` per 64 KiB, `reserve_pages` per 16 pages, the `bootlog` drain loop. | `core/task.c`, `task.h`, the listed files | Medium: `audio-refill` must never trip it (its service is ≤1 ms, `audio_dev.c:348-350` records `max_service_us`); the lock-hold rule needs a per-task hold count | Extracted `rq_push`/`rq_pop` as in `tests/host/usb/storage_run.sh:25-38`: a spinning device task plus a P_NORMAL desktop, desktop dispatched ≤20 ms; `audio` host suite unchanged |
| C6 | No `bootlog_sync` on device workers. The bind posts a flush request to `storage-writer` and waits (`kwait`, deadline) only for the terminal record; stage records stay in RAM. Coordinate with f1-62 (owner of `usb_storage.c`) and f1-63 (`mount.c`). | `usb/usb_storage.c`, `fs/mount.c`, `core/bootlog.c` | Medium: durability of bind records before a crash; keep one sync at the terminal stage | `tests/host/usb/bind_freeze_test.c` (production queues): `other_turns` advances during the bind; bind finishes inside 5 s with the writer holding the VFS lock |
| C7 | One `usb-host` task per controller (hosts are already independent: per-host `transfer_lock`) | `usb/usb_core.c:617-670` | Medium: 3 kernel stacks; shared `hosts[]` creation order | `tests/host/usb/service_test.c` modes plus a new mode: a blocked bind on host 1 does not stop host 0 scans (4/s) |
| C8 | Spawn and ELF: chunked zeroing and copy (C5 points). Later, demand-zero BSS (fault-in) to remove the 4 MiB up-front cost. | `proc/uaddr.c`, `proc/elf.c` | Low for chunking; demand-zero is high (fault path, ledger) | `tests/host/proc/launch_test.c`, `proc_test.c`: preemption points called ≥ bytes/64 KiB; ledger unchanged |
| C9 | Desktop UI thread at P_INTERACTIVE, workers at P_NORMAL (BeOS display-above-normal; `execution-abi.md:222-223`). Set by the supervisor for the desktop's first thread. | `proc/supervisor.c`, `proc/thread.c` | Medium: an interactive busy loop; the 1 s starvation guard covers normal tasks | Scheduler replay: desktop dispatch ≤ one quantum with two busy P_NORMAL workers; `tests/host/proc/desktop_test.c` |
| C10 | 1 ms pollers to deadline sleeps: `storage-writer` sleeps until the earliest writeback or log drain due (≤250 ms); `usb-hid` on the 8 ms HID interval | `fs/mount.c:586-601`, `usb/usb_core.c:623-634` | Low-medium (HID latency) | `tests/host/usb/latency_test.c`; storage writeback tests in `tests/host/fs` |
| C11 | Lazy CR3 for kernel tasks (`active_mm`): skip `write_cr3` when switching to a kernel task, with a reference on the borrowed address space | `core/task.c:323-325`, `vmm.c` | Medium (`as_destroy` while borrowed). Only if C3 shows switch cost. | Replay: borrowed directory freed only after the last kernel task leaves it |

Order: C1, C2, C3 land first (cheap, and C3 then measures the rest on the T23). C4-C8 follow, prioritized by the C3 records. C9-C11 last.

## 5. Kernel "cpu share" record (every 2 s)

The `cpustat` P_DEVICE thread sleeps 2000 ms. It is emitted always in verbose and test boots. In normal boots it is emitted only when a threshold trips (stretch >20 ms, device overrun, `need_resched` latency >20 ms, or a desktop stall record, which forces immediate emission). Each line is ≤240 B (the `klog` buffer is 256, `output.c:31`), sent as `klog_periodic` (ring, not protected prefix) and serial-buffered (C1).

```
[cpu] window_ms=2000 idle_pm=<n> irq_pm=<n> device_pm=<n> interactive_pm=<n> normal_pm=<n> switches=<n> boosts=<n> overruns=<n>
[cpu] top=<name>#<id>:<pm>k<kpm>s<max_stretch_ms>,... (5 tasks by CPU delta)
[kernel] stretch_max_ms=<n> task=<name>#<id> eip=<hex> resched_lat_max_ms=<n> resched_task=<name> irqoff_max_us=<n> timer_gaps=<sw>/<unclassified>
[kernel] long_paths serial_spin_ms=<n> serial_drop=<n> console_repaint_ms=<n> bootlog_drain_ms=<n> bootlog_queued=<bytes> elf_load_ms=<n> zero_pages_ms=<n> work_max_us=<n>
```

The fields come from the following sources:

- **Per-task counters** (new fields in `struct task`, `task.h:29-55`):
  - `run_cycles`: charged in `schedule()` before `switch_context` (`task.c:331`).
  - `kernel_ticks` and `user_ticks`: sampled in `timer_irq` via `tf->cs & 3`, with `EFLAGS.VM` counted as guest/user.
  - `stretch_start`: TSC at dispatch, reset at return to ring 3.
  - `max_stretch` and `stretch_eip`: the first timer IRQ in ring 0 beyond 20 ms stores `tf->eip`, which `addr2line` resolves against `VMM.ELF`.
  - `window_base`: snapshot for deltas.
- **Exited tasks** are accumulated into an `exited` bucket at `task_reap` (`task.c:445`), so per-mille values still sum to 1000.
- **Remaining fields:**
  - `kpm`: kernel-mode share of that task.
  - `irq_pm`: TSC spent in `handle_irq` (bracketed by `crit_begin`/`crit_end`, `trap.c:111-116`).
  - `resched_lat_max_ms`: time from the tick that first set `g_need_resched` to the next `schedule()`. This is the true preemption latency the desktop sees.
  - `irqoff_max_us` and `timer_gaps`: from `timing_snapshot` (`timing.c:90-98`).
  - `long_paths`: per-site TSC sums wrapped around A1, A3, A7, A11 and A12.
- **Cost:** one `rdtsc` and two adds per switch, one compare per tick, one walk of `all_tasks` every 2 s.
