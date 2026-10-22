# f2-96 — one pointer on screen; the ThinkPad volume keys reach the mixer again

Directive: [f2-96](../directives/f2-96-cursor-duplicate-ec-volume-keys.md).
Written 2026-10-22 by the implementer (Claude agent, worktree `wt/f2-96`).
Host evidence only; the T23 trial belongs to the lead.

## Evidence

- T23 image a4d9454e, disk log (`build/codex/t23-a4d9454e-BOOT.LOG`,
  5,644 lines): `[accel] savage mapped mmio=f861e000 cursor=f869f000` at
  lines 210, 961 and 1194 (boot, then two DOS full-screen transitions that
  stopped and restarted the engine), `[ec] thinkpad poll_ms=0
  source=default` (line 101), `[ec] thinkpad volume=1` at boot and
  `volume=4` only after the popup refresh (line 573), two `[ec] thinkpad io=9
  transactions=1` windows in the whole session. Owner: the hardware and the
  software cursor were both drawn a few pixels apart; the volume keys no
  longer changed the system volume.
- T23 image 5a10eef4 (1 Hz EC poll), serial copy
  `build/lead-scratch/t23-5a10eef4-serial.txt`: 14
  `[sched] audio_refill_overrun task=audio-refill#17 count=1
  consumed_us=2182..2227` records, each one 2.1 ms SMI plus 80–130 µs of
  refill work charged to the refill's 2 ms own budget; all 33 transport
  records `underruns=0`. The `irqoff_long` records of the same session put
  most blackouts inside `schedule()` in bounded phases (`queue` 29, `proc` 5,
  `pick` 5, `fpu` 1, `other` 3), mostly `from=device-work`.

## Sources

- savage2d.c (this tree): `cursor_set` hides the cursor (`cursor_show(false)`)
  before loading the shape; `cursor_show` refuses SHOW without a loaded shape;
  `fault`, `stop_engine` and `savage2d_quarantine` clear CR45 bit 0.
- fbdev.c `fbdev_accel_request`: EBUSY while `changing` or while another
  presenter holds the drawing claim (`present_begin`; f2-92 keeps the claim
  across its 1 ms in-flight slices), EAGAIN for a stale generation, both
  before any device access; `fbdev_change_begin` stops the engine (cursor
  hidden) before a new generation; `display_present.c` hides the cursor
  before a Win32 lease.
- [Linux ThinkPad ACPI documentation, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/Documentation/admin-guide/laptops/thinkpad-acpi.rst),
  volume control: the IBM console amplifier "is after the main AC97 or HDA
  mixer in the audio path, and under exclusive control of the firmware", and
  the firmware handles the volume keys on IBM models without the OS.
- [Intel 8259A PIC, order 231468-003](https://pdos.csail.mit.edu/6.828/2005/readings/hardware/8259A.pdf):
  the IRR holds one bit per interrupt level requesting service, so PIT edges
  during an SMI collapse into one request delivered after RSM
  (`pic_pit.c` already notes that the PIC cannot count missed periods).
- f1-100 validation: two SMIs per EC read transaction (GPI[12] routed to
  SMI#), 2,086–2,111 µs each, `period_ms=2/2` between the pair.

## Changes

1. Pointer (`apps/desktop/compositor.c`, `desktop.h`, `desktop.c`,
   `win32_presenter.c`): see [savage-2d f2-96](../design/savage-2d.md#f2-96-one-pointer-on-screen).
   Root cause: any failed cursor request or QUERY dropped the hardware owner
   without hiding the hardware cursor; EBUSY/EAGAIN are answered without
   touching it, so the frozen hardware cursor and the software pointer stayed
   on screen together. Now busy/stale answers keep the owner and repeat,
   other refusals hide first, engine loss and new generations clear the
   visibility, the engine takes the pointer back after a client pointer or
   (after 1 s) a refusal.
2. EC poll (`drivers/thinkpad_ec.c`, `include/ciuki/thinkpad_ec.h`): default
   one RD_EC every 2 s (`THINKPAD_EC_DEFAULT_POLL_MS`), `ecpoll=0` off,
   `ecpoll=1` 1 Hz; `[ec] thinkpad io=` records unchanged. Loader: comment
   only (`menu.inc`); both tokens were already accepted; 791 bytes free
   before and after (`scripts/loader_budget.py`). The codec is not given the
   mirrored level (the Linux source above: the amplifier is the master).
3. Firmware time (`core/cpustat.c`, `include/ciuki/{task,cpustat}.h`,
   `audio/audio_dev.c`): proven SMM time is left out of the scheduler
   budgets and of the audio service's 1 ms yield, 2 ms deadline and
   `max_service_us`; `run_cycles` and class totals stay raw. Proofs:
   (a) IF clear: a gap of at least 1 ms between two marks of bounded phases
   (`cpu_irqoff_bounded`, the f1-94 rule), credited to the interval being
   charged or, after the section's last charge, to the running task;
   (b) IF set: a PIT entry more than 2 ms after the previous one (not from a
   V86 frame) is credited from the missed edge (from the PIT phase of the
   last on-time entry) to the entry, after the last IF-clear section end and
   the task's last accounting point. Record per window with credit:
   `[sched] firmware_credit count=<n> us=<n> max_us=<n>`.

No new panic path; no change to SMI enables, GPE routing, the loader binary
or the ABI.

## Host evidence (2026-10-22, nice -n 19, one at a time)

- `tests/host/desktop/test_accel.py` `test_pointer_goldens`: production
  compositor against a Savage cursor model; 20 golden frames
  (`pointer-goldens.json`): software (engine not ready), late readiness,
  hardware motion without scanout upload, busy MOVE held then retried, busy
  shape change held then retried, busy QUERY (no engine-loss notice), client
  pointer with a busy hide then drawn, engine back after the client pointer,
  engine loss, engine back, refused MOVE (hide first), no re-arm before 1 s,
  re-arm after 1 s, refused MOVE with a busy hide, engine loss reported by
  MOVE, new generation, window move under the hardware pointer. After every
  frame the scanout equals the scene with or without the software pointer,
  never a software pointer beside a visible hardware cursor, and in steady
  state exactly one pointer where the software pointer belongs.
  Negative control: the pre-f2-96 `compositor.c` (HEAD) with the same test
  fails at frame 04: `hardware and software pointer both visible`.
- `tests/host/t23_audio_replay_test.c --smi=…` (run by
  `test_kernel_latency`), 400 s continuous T23 replay with the production
  scheduler, sampler, mixer and refill, 2.1 ms blackouts (DMA running, PIT
  edges latched once), lead 8 periods of 256 frames (42.7 ms):

  | blackouts | placement | lead min | refill own overruns | deadlines | result |
  | --- | --- | --- | --- | --- | --- |
  | 2.1 ms at 1 Hz | inside the refill | 31.6 ms | 0 | 0 | PASS |
  | 2.1 ms at 1 Hz | any task | 31.3 ms | 0 | 0 | PASS |
  | EC pair every 2 s (default) | any task | 29.1 ms | 0 | 0 | PASS |
  | EC pair every 2 s | device worker | 29.6 ms | 0 | 0 | PASS |
  | EC pair every 1 s (`ecpoll=1`) | device worker | 29.6 ms | 0 | 0 | PASS |
  | EC pair every 2 s | both inside the refill (stress) | 29.5 ms | 15 | 0 | reported |
  | 2.1 ms at 1 Hz, no proof (V86 tick) | inside the refill | 31.6 ms | 472 | 400 | negative control |

  No underrun, PCM starvation or DMA overtaking the published frames in any
  run. Without the proof the model reproduces the 5a10eef4
  `audio_refill_overrun` records (`refill_service_max_us=2220`). The stress
  case is unprovable time before each missed PIT edge (up to one period per
  SMI); on the T23 the SMI follows the EC worker's own port access by
  13–15 µs, so both SMIs of a transaction land in that worker or in its
  `schedule()` (bounded phases, proven exactly). With the device-worker
  placement the model's busy device peer records `device_overrun` (173 per
  400 s, 200 without the proof): f1-90 demotes only that consumer.
- `tests/host/scheduler_irqoff_test.c`: both proofs on the real
  schedule/timing/cpustat hooks, including the EC pair's second SMI
  (1,250 µs proven from the PIT phase, 850 µs unprovable), resumed-caller
  gaps, V86 frames and IF-clear delays still charged.
- `tests/host/audio/thinkpad_ec_test.inc`: 2 s default, 1 Hz with
  `ecpoll=1`, off with `ecpoll=0`; one transaction (9 port accesses) per
  2 s window over 60 s, a key press mirrored by the next poll, no EC write
  from the poll; native start records.

## Expected T23 records

- `[ec] thinkpad poll_ms=2000 source=default`; one `[ec] thinkpad io=9
  transactions=1 … poll_ms=2000` per 2 s window; a volume key changes the
  tray within 2 s and logs `[ec] thinkpad volume=<n> mute=<m>`.
- `[kernel] smi … gaps=2 rate≈1/s` and `smi_coincide … ec_near=2 …
  ec_poll=2000` per window, as on 5a10eef4 at half the rate.
- `[sched] firmware_credit count=… us=…` in those windows; no
  `[sched] audio_refill_overrun`; `[audio] transport … underruns=0`.
  Possible: `[sched] device_overrun task=thinkpad-volume#…` (the worker
  alone).
- `[desktop] accel … pointer=hw pointer_retries=<n> pointer_fallbacks=0`;
  one pointer on screen, also across DOS full-screen transitions and Win32
  windows with their own cursors.
