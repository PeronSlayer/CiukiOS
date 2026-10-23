# fx-22: system hang after Quake (full-screen return)

Written 2026-10-23 for directive fx-22 (`docs/directives/t23-f441c5bd-trial-fixes.md`),
from the T23 trial of preview `f441c5bd` (source `18dcfcb7`). Evidence is
private and cited by line: `legacy/local/physical/f441c5bd/disk/LOGS/BOOT.LOG`
(`B:n`), the earlier trial `legacy/local/physical/e831e685/` (`E:n` for
BOOT.LOG, `ES:n` for serial.log). Design amendment:
[supersavage-fullscreen.md, fx-22](../design/supersavage-fullscreen.md#fx-22-return-transaction-amendment-2026-10-23).

## Records

| where | record | meaning |
| --- | --- | --- |
| B:7380-7406 | `fullscreen_request … mode=19` … `first-guest-frame tick=324258` | Quake enters native full screen (mode 13h) |
| B:7391-7393 | `native-save-planes 323656` → `native-streams-off 323878` | the entry's 256 KiB planar save: 222 ms (0.85 us per byte) |
| B:14121-14147 (bank n=51) | `vm_perf_page … pinned=6131` (n=50) → `pinned=0`; `vector=fe calls=316 us=922142`; `path=bios vector=10 calls=5 us=828249` | Quake quits: config written through INT FEh, five INT 10h calls (its VID_Shutdown mode 3 and end screen) through the full-screen transfer, the DPMI client's locked memory released |
| B:14074 | `[audio] transport idle-stop … sources=0` | Quake's sound shut down |
| B:14152-14154 | `perf … exits=560 … busy_pm=807`, `perf_cost cause=rm_int exits=160 us=1644894` | the last 2 s: the VM inside real-mode services 1.6 s |
| B:14155-14158 | `fullscreen_watchdog … cause=hotkey`, `watchdog-return 561804`, `guest-revoke 561804`, `vga-capture 561816`; end of file | Alt+Enter; the VM parked in 12 ms; nothing after the capture began |
| E:1604-1622 | `guest-revoke 151687`, `vga-capture 151795`, `desktop-restore 154025`, `desktop-restored 154159`, `lease_return_ms=2516` | a completed return whose capture took **2230 ms** |
| E:1513-1516 | `native-save-planes 129173` → `native-streams-off 129399` | the same 256 KiB at entry: 226 ms |
| ES:6214-6216 | `[cpu] window_ms=2714 idle_pm=0 … normal_pm=795`, `top=dosvm#59:418k418s8,desktop#40:338k337s8,audio-re#18:56k,vm-clock#58:41k,device-w#9:34k` | during that capture the display worker is not in the top five (< 34 permille, under 92 ms in 2.7 s); desktop worker 40 (base 7, desktop share group) 338 |
| other trials | vga-capture → desktop-restore 170-800 ms (9af46936 308, 82fadce3 327, 423b803a 288/356, 4bcd1d43 330, c2d2a237 359, b453bc92 688, a4d9454e 800, d598bf4c 170) | the usual return |
| 041b221f (f1-80) | DOOM quit in native full screen, log ends at `native-restore-streams` | an earlier hang after a game exit to text mode in full screen |

The step that did not complete is `vga-capture`: its successor
`desktop-restore` is the first record of `display_fullscreen_end`, and the
code between them is `vga_hardware_capture` (256 KiB through the uncached
aperture) and two VM device resets. The return checkpoints were only queued
(f1-80), so the last durable record could precede the actual stop by one step.

## Windows 98 reference

- RBIL INT 2Fh [4005h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4751.htm)
  and [4006h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4752.htm):
  the video state is saved when a DOS box goes to the background and
  restored when it returns to the foreground.
- [OS/2 Museum, Windows 3.x VDDVGA](https://www.os2museum.com/wp/windows-3-x-vddvga/):
  the VGA VDD demand-pages video memory and "must track video memory usage"
  (KB80901); a display driver re-establishes the register state and forces a
  repaint on return, which "avoided the need to save any video memory".
- Win95 DDK VMM (already cited by `dosvm_clock.c` and the f1-98 desktop
  share): Adjust_Exec_Priority / Time_Critical_Boost run time-critical work
  above every time slice; VPICD simulates no interrupt into a suspended VM; a
  VM's VxD service is not interrupted by a focus change.
- [Intel SDM Vol. 3A, 4.8 Accessed and Dirty Flags](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  the processor sets a PTE's D flag on the first write through it; software
  that clears it must invalidate the TLB entry (here the VM's CR3 load or
  INVLPG in the VM's context).
- [FreeVGA sequencer memory mode](http://www.osdever.net/FreeVGA/vga/seqreg.htm#04)
  and graphics miscellaneous register: chain-4 and odd/even select the plane
  from the low address bits without moving the offset's 4 KiB page; GC06
  selects the A0000h/B0000h/B8000h map.

## Root causes (pre-fx-22 code, `6b0a5481`)

1. **The return transaction had no priority and no deadline**
   (`dosvm_fullscreen.c` leave, `display.c` end, run on `display-watchdog`,
   P_NORMAL level 7). `task.c rq_pop` serves the f1-98 desktop share, the
   focused VM and every ordinary level above 7 (focus +2, I/O +1, the VM's
   inherited 9) before it; the 100 ms starvation boost is its only
   guarantee. The capture loop yields every 256 bytes. Production-scheduler
   replay (`tests/host/t23_fullscreen_return_test.c`): a desktop worker busy
   90 % at level 8 stretches the 220 ms capture to **2661 ms** (trial
   2230 ms), the display worker at 86 permille; one CPU-bound level-9 task
   and the pre-fx-22 return **does not finish in 30 s** (10 permille): the
   screen stays on the DOS text mode, which is the f441c5bd symptom.
2. **The parked VM polled every millisecond** (`dosvm_fullscreen_checkpoint`,
   `task_sleep_ms(1)` loop): a suspended VM stayed runnable, the focus VM of
   `rq_pop` and, with IRQ0 requested, urgent in `kpreempt_point`; 1000
   dispatches per second with a CR3 switch each, against Windows' suspended
   VM. The replay shows the priority (1), not these wakes, decides the delay
   (same results with and without a pending IRQ), so this is a model fix.
3. **Alt+Enter tore the guest's own mode set** (`park` set `fullscreen_hold`,
   `fs_access` denied the VM's synthetic VGA BIOS continuation at once, though
   park waits for the VM anyway). In f441c5bd Quake was quitting through five
   full-screen INT 10h calls. Replay `CIUKI_VM_FX22=transfer` on `6b0a5481`:
   the adapter is left in the transfer's planar state, blanked (SR01 bit 5,
   GC06=05h), the capture copies that into the model, and the DOS prompt's
   text mode is lost (`model_text=0 adapter_text=0 model_blank=1 gc6=05`).
4. **The capture read all 256 KiB every time, unbounded** (`vga_hardware_capture`).
   Replay `deadline` on a stalled 10 us/byte aperture: 2781 ms, no fallback.
5. **Closing the VM during the return** (`dosvm_destroy`): the hold check ran
   only on entry; a CLOSE whose stop starts the return reaped the task,
   closed UART/ports/network/CD audio, then failed at the display step.
   Replay `closing` on `6b0a5481`: `destroy=-16`, `failure
   stage=destroy-display`, VM left STOPPING with its devices closed; the
   window answers "Game is still running".
6. **Kernel copies bypassed the owner rule**: `dosvm.c transfer()` used the
   physical aperture for any task while a VM was full screen, without the
   `fs_access` check, and its writes were invisible to any capture policy.

Not established from the records: whether the T23 was still scheduling
after `vga-capture`. With (1) alone the device-class storage writer would
keep publishing periodic records; BOOT.LOG has none, so either a lock the
bootlog drain only try-takes was held by a starved task, or the CPU/bus
stopped (an uncached aperture cycle that never completes). The return's step
records are now durable before each next step and the stall reporter runs on
a device-level task, so the next trial names the step and whether the system
was alive. The reserved-sector raw trail (black box, sectors 8-160) of the
f441c5bd disk was not in the evidence; if the T23 has not booted since, it
may hold records after `vga-capture`.

## Fixes

- **Transaction boost** (`boost_begin/end`): entry and return run at
  `SCHED_BOOST_CAP` (15), above the desktop share and the focused VM, below
  device service; the previous class and configured level are restored.
- **Parked VM blocks** on `v->wake` (unhold, stop and focus resume wake it;
  20 ms re-check), not a 1 ms poll.
- **No torn transfer**: `fullscreen_revoked` replaces `fullscreen_hold` in
  `fs_access`; park lets an in-flight synthetic VGA BIOS transfer finish
  (`transfer_vm`, bound 750 ms instead of 250 ms), then revokes; stop, VBE
  mode change and park timeout revoke at once.
- **Dirty-page capture**: the granted PTEs' D bits accumulate in
  `fullscreen_dirty` (1 s sampler, return, stop); a completed BIOS transfer
  clears them (model and adapter agree again); the owner's kernel copies mark
  their pages. Guest page g maps to plane pages g&15 and (g+8)&15 (A0000h and
  B8000h maps, chain-4 and odd/even keep the page).
  `vga_hardware_capture_pages` reads registers, the selected pages page by
  page, then FFFFh once for the latches. The same capture serves the
  full-screen INT 10h transfer (`model_begin`).
- **Capture bound** 750 ms, checked between pages; on expiry the captured
  pages are kept, the VM stays suspended with `-ETIMEDOUT` (the f1-80 park
  fallback) and the desktop is restored.
- **Durable steps**: return checkpoints wait at most 50 ms each, 200 ms per
  return, for the storage worker's commit (`bootlog_sync` with a deadline),
  then stay queued; entry keeps its synchronous gate; new step `vga-captured`.
- **Timing records**: every `fullscreen_step` carries `step_ms`; one
  `[dosvm] fullscreen_return vm= generation= park_ms= park= transfer=
  capture=complete|partial|skipped capture_pages=n/m capture_ms= restore_ms=
  total_ms= result=` per return, before the existing `fullscreen=0` record.
- **Stall report**: cpustat (P_DEVICE, each second) records
  `[display] fullscreen_stall step= step_ms= return_ms= task= state= level=`
  once a return step lasts 1 s, every 5 s after, and requests its commit.
- **Teardown**: `dosvm_destroy` waits up to 3 s for a pinned VM before any
  side effect, finishes or waits for the display release right after the VM
  task ends and before reaping/closing; the transaction's own task gets
  `-EBUSY` at once.
- **Aperture rule**: `transfer()` uses the physical aperture only through
  `dosvm_fullscreen_aperture` (owner continuation before revoke, or the switch
  service); other tasks use the model.

## Host replays and measurements

`CIUKI_VM_FX22=transfer|deadline|closing|dirty build/host/dosvm_test` (the
production state machine, coroutines at kpreempt_point/task_sleep_ms):

| replay | `6b0a5481` (`-DFX22_HEAD_BASELINE`) | fx-22 |
| --- | --- | --- |
| transfer | exit 1, `FAIL 147`: `model_text=0 adapter_text=0 model_blank=1 gc6=05` | PASS: text mode kept, `transfer=1 park=parked capture=complete` |
| deadline | exit 1, `FAIL 171`: 2781 ms, 262144 bytes, not suspended | PASS: 979 ms, 5 of 16 pages, suspended, `-ETIMEDOUT` |
| closing | exit 1, `FAIL 214`: `destroy=-16 destroy_display_failures=1 vm_state=4` | PASS: `destroy=0`, VM DEAD, no failure record |
| dirty | (fx-22 only) | PASS: one B8000h store reads 2 pages (32769 bytes); a BIOS transfer leaves 0/0 |

Production-scheduler replay `t23_fullscreen_return_test.c` (capture 256 KiB
at 860 ns/byte, restore 133 ms, return at 1 s, give up at 31 s):

| scenario | pre-fx-22 total (capture) | fx-22 total (capture) |
| --- | --- | --- |
| desktop worker 90 % at level 8 (e831e685) | 4128 ms (2661), display 86 permille | 396 ms (249) |
| desktop worker CPU-bound at level 7 | 713 ms (447) | 396 ms (249) |
| same as the first, no pending VM IRQ | 4128 ms (2661) | 396 ms (249) |
| CPU-bound level-9 task beside the worker | not complete in 30 s, display 10 permille | 399 ms (251) |
| idle desktop | 394 ms (247) | 394 ms (247) |

These are host models (the stated workload assumptions are in the test's
header), not T23 measurements. The f1-80 replays keep their numbers once the
fixture marks every guest page written (684/784/410 ms).

## What the next T23 trial must show

- After Quake (or DOOM) quits to the prompt in full screen, Alt+Enter brings
  the desktop back; the DOS window shows the prompt in text mode.
- BOOT.LOG: `fullscreen_return … park=parked capture=complete` with
  `total_ms` under 1000 and `capture_pages` small for a text prompt
  (a static prompt: 2/2 or 0/0); every `fullscreen_step` has `step_ms`, the
  return steps through `desktop-restored`.
- No `fullscreen_stall`. If one appears, it names the step, the task and its
  level; if the log ends without it, the stop was not a scheduling stall.
- Closing a DOS window during a return gives no `destroy-display` failure.
