# T23 DOS window lag (trial c42f556e, 2026-10-25)

- Written 2026-10-25 19:16-20:35 CET in `CiukiOS-wt-t23-dosperf` (branch
  `wt/t23-dosperf`, from `main` 50a843517). Host evidence only: no QEMU,
  image build or hardware run. Every T23 figure below is a record of the
  trial (image c42f556e, `main` a347ca76b) or a model stated as such.
- Owner report: Warcraft II starts in the window and lags "like every DOS
  game", a complaint repeated for a week. Machine: Pentium III-M (TSC
  1.0 GHz in this log), S3 SuperSavage IX at 1024x768x32, AC'97.
- Evidence: the trial's `BOOT.LOG` (private, `legacy/local/physical/`); only
  record lines are quoted. Generation 4934 is the Warcraft II demo (catalog
  game 4), generation 7676 DOOM (game 2).

## 1. What the trial shows

### 1.1 Warcraft II: the monitor, not the game, uses the CPU

`vm_perf_cpu`/`vm_perf_share`/`vm_perf_exits`/`vm_perf_remap`/`vm_perf_video`
per 5 s bank (rate 35 Hz is mode 13h, 60 Hz the VESA 101h mode; frames 0 is
full screen):

| bank | guest | monitor | off CPU | exits/s | monitor us/exit | lean | frames | rate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| n=3 | 8.1 % | 54.3 % | 37.6 % | 23,764 | 22.9 | 3.4 % | 23 | 35 |
| n=4 | 9.2 % | 46.5 % | 44.4 % | 21,505 | 21.6 | 0.0 % | 12 | 35 |
| n=5 | 12.4 % | 53.6 % | 34.1 % | 21,487 | 24.9 | 0.1 % | 2 | 60 |
| n=6 | 12.2 % | 34.1 % | 53.6 % | 45,051 | 7.6 | 74.0 % | 22 | 60 |
| n=7 | 6.8 % | 64.7 % | 28.5 % | 19,491 | 33.2 | 0.0 % | 0 | 60 |
| n=8 | 8.6 % | 61.3 % | 30.0 % | 30,513 | 20.1 | 0.0 % | 0 | 60 |

The game itself got 7-12 % of the CPU; the virtual machine monitor 34-65 %.
Where the monitor time went (`vm_perf_cost`, `vm_perf_port`, `vm_perf_vector`):

- **PM CLI/STI**, n=4: `cause=pm_flags exits=50715 us=1016335`, 20 % of
  the bank at 20.0 us each; n=7: `cause=pm_flags exits=13750 us=1543733`
  (112.3 us each, the CLI proofs' decodes).
- **PIT reads**, n=4: `cause=pit_pic exits=38261 us=774032` (20.2 us):
  `port=0040 exits=24776`, `port=0043 exits=12388`, i.e. latch + two reads
  2,478 times a second, from DOS/4GW's INT 21h 2Ch handler
  (`path=pm vector=21 calls=11996`).
- **PM software interrupts**, n=4: `cause=pm_int exits=11999 us=301310`
  (25.1 us).
- **Palette loads**, n=6 (window, VESA): `port=03c9 exits=184514
  us=1043347` (5.7 us, 21 % of the bank) with `lean=166640 dac=0`;
  `dac=0` in **every**
  bank: the fx-27 DAC burst never ran. The demo reloads all 256 DAC entries
  every retrace (768 OUTs).
- **Music**, n=3: `port=0220 exits=11025 us=142536`, `port=0222
  exits=11130` (12.8-12.9 us): OPL3 writes through the Sound Blaster's 220h/222h, not lean.

The cause of the missing lean exits is in the protected `[vm-audio]` line:

    [vm-audio] vm=1 cadence_clock ... irq0_lost=74 ... cli_hits=457 cli_learned=4 cli_missed=9232 seeds_skipped=18
    [vm-audio] vm=1 cadence_clock ... irq0_lost=116 ... cli_hits=536 cli_learned=4 cli_missed=20804 seeds_skipped=18
    [vm-audio] vm=1 cadence_clock ... irq0_lost=280 ... cli_hits=624 cli_learned=4 cli_missed=25770 seeds_skipped=18

A CLI that "misses" its fx-16 proof restarts the boundary-seed demand for
1 s (`dpmi_seed_wanted`), and no exit is lean while seeds are wanted. With a
miss every ~0.5 ms the demand never ended: every exit took the ordinary path
(20-25 us) and also seeded the next CLI proof.

### 1.2 DOOM: settled, but still exit-bound

| bank | guest | monitor | off CPU | exits/s | lean | flips | frames | `db` exits | `vga_io` us |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| n=3 | 35.8 % | 28.7 % | 35.5 % | 22,242 | 69.6 % | 137 | 13 | 11,906 at 37.0 us | 7.1 |
| n=4 | 21.3 % | 39.0 % | 39.8 % | 34,447 | 79.1 % | 90 | 33 | 12,718 at 37.3 us | 7.4 |

DOOM's CLI sites are proven (`cli_hits=3472..5447 cli_missed=0`,
`seeds_skipped` 52,648 -> 253,271), so its port exits are lean, at
6.0-7.4 us each (`0388` 6.0-6.5, `0020` 6.6-6.8, `03c5` 7.1-7.4). Its
remaining monitor costs are Mode Y's per-column plane OUTs (inherent to a
trapped VGA, as in Windows 98), the return-tracker checkpoints (`db`, 9 %)
and `pm_flags` (7,501-7,843 per 5 s at 17.3-17.8 us on the ordinary path).

### 1.3 Presentation and off-CPU time

DOOM rendered 90-137 page flips per 5 s but the window presented 13-33
frames (`vm_perf_video flips=137 frames=13`, `flips=90 frames=33`); a frame
is counted only when rows changed, so the log cannot tell a still view from
a starved presenter. The VM was off the CPU 28-54 % of the time, but this
non-verbose boot emitted no `[cpu] window` records during the sessions
(cpustat emits them only on an overrun, at most once a minute), so the log
does not say who used that time. It was not the Win32 shared-page tick work
that `wt/t23-render` found (device-work up to 274-342 permille,
`[work] budget exceeded site=c01485f0`): no Win32 program ran during either
DOS session (the first, `[win32] start ... Q3ADemo.exe`, comes after the DOOM
session's `lifecycle stage=cleanup`, and `site=c01485f0` appears only after
it), and the cpustat windows just before and just after the Warcraft II
session show device-work at its idle level (`[cpu] top=...
device-w#9:46k46s0` and `device-w#9:53k53s0`: 46 and 53 permille).

The kernel half of a windowed present cost 10.4 ms per frame for Warcraft
II in VESA (`vm_perf_present ... n=6 present_us=229022 ... rows=9398`, 22
frames: every retrace's palette change redraws all rows) and 3.0-6.7 ms for
DOOM (n=4 `present_us=100033`, 33 frames; n=3 `present_us=87456`, 13
frames); the DOS Box copy and the desktop composition come on top. The DOS
Box presenter is an ordinary base-7 thread: the focused VM has the
scheduler's preference for 8 of every 10 ordinary ticks (`task.c`
`FOCUS_BUDGET`), the desktop group 25 % (f1-98), and a ready presenter
gets at most 2-tick slices beside a ready VM. Whether that starves the
window's frames cannot be read from this log; the new `vm_perf_presenter`
record (section 4, item 6) measures it at the next trial.

## 2. Why the earlier attempts did not remove it

- **fx-27 (windowed DOS graphics)** made port exits lean and palette loads
  one exit, but only for an exit with no seed demand and, for the bursts,
  no armed fx-16 flags image. Warcraft II never satisfies either: its CLI
  misses keep the seed demand on (1.1), and it keeps one flags image armed
  for its whole run (an enclosing DOS/4GW region; `if_armed` was 1 at every
  second of the host replay below), which the I/O, DAC and VGA bursts
  refused. fx-27 noted the second limitation itself ("A stale armed image
  also keeps the I/O bursts and IF-recovery stepping off") and left it.
- **fx-16 (CLI proof cache)** was derived from DOOM/DMX, whose saving CLIs
  follow PUSHF directly. Warcraft II's event loop saves the flags into a
  local and restores them with STI, so its CLI can never be proven and was
  re-decoded and counted as a miss on every execution.
- **PM CLI/STI** had no lean path at all; DOOM has ~1,500 a second,
  Warcraft II ~10,000.
- **101-dosvid/101-dosvid2** worked on the presenter's copies and the
  full-screen LFB; **fx-9** (VM clock task) and **fx-23** (storage waits)
  on clock starvation and disk waits. None of them reduces the monitor's
  34-65 % in this trial.
- Every exit-cost replay so far was DOOM or Quake. Warcraft II arrived on
  2026-10-25 and had no performance replay; the host replay below now
  reproduces its CLI misses (1,882 per simulated second, hits stalled at
  a few hundred, 5 sites learned; trial: `cli_missed` 9,232 -> 25,770, hits
  457-624, 4 learned), its zero lean share and its zero DAC bursts.

## 3. Windows 98 research

- **Window update rate.** Windows 3.1/9x update a DOS window from the VDD's
  shadow on a timer, `WindowUpdateTime` in `SYSTEM.INI [386Enh]`, default
  50 ms ("the amount of time (in milliseconds) Windows takes before it
  updates the display of non-Windows applications running in a window"):
  [powernet SYSTEM.INI reference](http://www.powernet.co.za/info/sw/win/sysini/386enh/125.htm),
  the same text as Microsoft KB 83436 / Windows 3.1 Resource Kit. The
  grabber repaints only what changed (Windows 3.0 DDK *Virtual Device
  Adaptation Guide* 18.4.1, Get_Mod/Clear_Mod, cited in
  `fx-27-windowed-dos-graphics.md`). CiukiOS samples at the guest's own
  refresh (35/60/70 Hz cap), already faster than 20 Hz.
- **Trapped ports cost speed in Windows too.** The PIF option "monitor
  ports" and `TrapTimerPorts` (VTD traps 40h-43h; `TRAPTIMERPORTS=OFF`
  speeds up games that read the timer) and `MinTimeSlice=20`:
  [COMPUTE! 143, August 1992](https://www.atarimagazines.com/compute/issue143/S1_Optimizing_Windows.php).
  Windows 98 keeps timer trapping on by default; the exits exist there as
  here, the difference is their cost.
- **Idle sensitivity / background.** Windows 98 Resource Kit (local copy,
  `build/rskt/wrkc27_3.htm`): to speed up an MS-DOS program, "Drag the Idle
  Sensitivity slider toward Low" and clear "Suspend Always" on the Misc tab.
  CiukiOS has both (G17 property pages); the trial's Warcraft II never went
  idle (`idle_*=0`), so idle detection was not the cause.
- **CLI in protected mode.** Windows virtualises IF for protected-mode code
  ([R. Chen, 2015-12-04](https://devblogs.microsoft.com/oldnewthing/20151204-00/?p=92281)),
  and its CLI trap handler "inspects the code preceding the cli
  instruction" for a PUSHF and copies the pushed IF into the pushed TF
  ([R. Chen, 2016-04-11](https://devblogs.microsoft.com/oldnewthing/20160411-00/?p=93281)).
  The inspection is of the immediately preceding code; a CLI after other
  instructions is simply not a saving site, which is rule 1 below.
- **VME/PVI.** Pentium virtual-mode extensions remove V86 CLI/STI faults
  (PVI the CPL 3 ones) ([Dr. Dobb's, 1998](https://jacobfilipp.com/DrDobbs/articles/DDJ/1998/9803/9803p/9803p.htm));
  `dos-dpmi-contract.md` keeps them disabled ([F3-F4]). Not proposed here.

## 4. Changes

1. **Unprovable CLI** (`dpmi_dispatch.c` `flags_save_adjacent`): every
   arming rule needs PUSHF or POP r (after PUSHF) to end at the CLI, so the
   byte at CS:EIP-1 must be 9Ch or 58h-5Fh. Any other byte: the CLI is
   refused at once, without a proof, a miss or a seed-demand restart.
   Arming outcomes are unchanged (the proofs always failed there).
2. **Lean PM CLI/STI** (`dosvm.c lean_io`, `dpmi_dispatch.c
   dpmi_lean_flags`/`pm_flags_insn`): the instruction exactly as `emulate`
   (one shared helper), then the boundary work of `dpmi_pm_trap`; if a CLI
   proof restarts the seed demand or return tracking, the ordinary tail runs.
3. **STI shadow** (`lean_io`): a PM shadow ends at the next #GP(0), as in
   `pm_trap`, so the exit after a lean STI can be lean; a pending IRQ is
   delivered by that exit's ordinary tail as before.
4. **Bursts beside an armed image** (`dosvm_vga.c video_run_blocked`,
   `dosvm_io_burst.inc burst_blocked`): an armed flags image no longer stops
   the I/O, DAC or VGA burst. Their decoders execute no POPF/IRET/POP/RET or
   stack transfer; the VGA and DAC bursts store only to the aperture, the I/O
   burst only through REP INS elements emulated as the ordinary path does.
   The image is not loaded by a burst, and `armed_observe` sees it at the
   next exit as after native execution; return tracking, watchpoints, TF, shadows,
   IF-recovery steps, deliverable IRQs and reschedules still stop them.
5. **Sound Blaster OPL3 ports lean** (`dosvm.c lean_port`): base+0..3 and
   base+8/9 reach `dosvm_opl_io`, like 388h-38Bh.
6. **Evidence** for the presenter: `group=vm_perf_presenter ... samples=
   waits= wait_avg_us= wait_max_us=` per 5 s bank (PRESENT samples, VIDEO
   wakes of WAIT_PRESENT and the delay from the frame becoming ready to the
   presenter running; `dosvm_client.c`, `dosvm_perf.inc`).

Design: `dos-dpmi-contract.md`, "t23-dosperf amendment" (proposed for the
lead's decision). Compatible with `wt/t23-war2fs` (VBE full screen lean
exits/DAC burst, `lean_before` and `dosvm_vga_dac_burst` gating lines): this
branch changes neither line; with both merged, the full-screen palette load
also becomes one burst, since the armed image no longer blocks it.

## 5. Host evidence

### 5.1 Warcraft II replay (the trial's figures reproduced)

The demo under DOS/4GW Professional 1.97 on the production VM code
(`tests/host/dos/dos_extenders.py` image, `tests/host/dosvm_guest.c` with
`GUEST_PLAY_PERF=1`, `GUEST_PERF=1 GUEST_T23=1 GUEST_T23_MODEL=fx27
GUEST_SLICE=200000`, Esc at 12 s and 14 s). Per simulated second:

| phase, per simulated second | `main` (a347ca76b sources) | this branch |
| --- | ---: | ---: |
| intro movie (13h, 6-11 s): exits | 17,651 | 32,998 |
| lean exits | 0 | 26,887 (81 %) |
| CLI proof misses / `unprovable` | 1,882 / - | 0 / 4,358 |
| seed demand on | every second | never |
| event-loop iterations (one candidate CLI each) | 1,882 | 4,358 |
| `pm_flags`, `pit_pic`, `pm_int` exits | 8,258, 6,214, 1,917 | 18,156, 9,174, 4,392 |
| title screen (101h, 16-19 s): exits | 77,616 | 13,445 |
| DAC exits / DAC bytes burst | 64,596 / 0 | 165 / 64,175 |
| armed flags images | 1 (whole run) | 1 (whole run) |
| guest instructions in 30 simulated s | 405,053,433 | 658,812,052 |

The `main` column is the trial's signature: CLI misses ~2,000 per second
(trial 9,232 -> 25,770 over the session, hits 457-624, learned 4 against
the replay's 520-593 hits, 5 learned), no lean exit, `dac=0`. The more
exits per second in the right column are the game doing more work per
simulated second: it gets the time the monitor no longer takes.

DOOM shareware (attract demo, same runner, 20-30 s, per simulated second):
lean exits 51,578 of 54,794 (94 %) -> 56,152 of 57,856 (97 %); ordinary
exits 3,216 -> 1,704 (its 1,561 PM CLI/STI now lean); page flips 29.4 ->
31.7 per second; guest instructions in 30 s 1,110,078,244 ->
1,285,344,990; same end state (`PLAY FRAME mode=13 320x200`, runner PASS).
DOOM's CLI sites stay proven (`missed` 3 -> 0).

### 5.2 T23 cost per event-loop iteration (model)

The replay's exit mix priced at the trial's per-exit costs: ordinary
`pm_flags` 20.0 us (17.5 us without proof misses, DOOM n=3/4), `pit_pic`
20.2, `pm_int` 25.1, V86 exits 10.8, host timer 51.8, other I/O 12.9 us
(Warcraft II n=3/n=4); a lean exit 6.6 us (DOOM n=3/n=4: 6.0-7.4 us).

| per event-loop iteration (intro) | `main` | this branch |
| --- | ---: | ---: |
| exits | 9.4 (0 lean) | 7.6 (6.2 lean) |
| monitor time at T23 prices | 185 us | 73 us (-61 %) |

| title screen, per second | `main` | this branch |
| --- | ---: | ---: |
| monitor time at T23 prices | 552 ms | 144 ms + DAC decode (-68 % or better) |

Priced exits: `main` intro 8,258 x 20.0 + 6,214 x 20.2 + 1,917 x 25.1 +
604 x 10.8 + 52 x 51.8 + 32 x 12.9 us = 348 ms per 1,882 iterations; this
branch 26,887 lean x 6.6 + 4,392 x 25.1 + 1,015 other CLI/STI and PIT x 17.5
+ 598 x 10.8 + 74 x 51.8 + 32 x 12.9 us = 316 ms per 4,358 iterations. For
the trial's bank n=4 (monitor 2,323 ms of 5,000, guest 9.2 %) the same
ratio leaves about 920 ms of monitor time; the 1.4 s freed per 5 s would
let the game run up to about 37 % of the time instead of 9 % (less if
other tasks take part of it). In the
window's VESA phase (bank n=6: 184,514 DAC exits, 1,044 ms) the palette
loads become one exit each, about 240 exits per 5 s. These are model
figures; section 7 lists the records that measure them.

### 5.3 Unit tests and benches

`tests/host/dosvm_t23dosperf_test.inc` (in `dosvm_test`, also alone with
`CIUKI_VM_T23DOSPERF=1`); production monitor, fake trap transport:

| case | `main` behaviour | this branch |
| --- | --- | --- |
| Warcraft II event loop (its CLI bytes, then the DOS/4GW time read CLI; OUT 43h; IN 40h; IN 40h; STI; STI), 1,000 iterations | 1,000 misses, seed demand on, 5,000 ordinary exits (`T23_HEAD`-style probe of the old CLI rule) | 0 misses, 1,000 `unprovable`, no seed demand, 5,000 lean exits (budget: 0 ordinary) |
| Warcraft II palette load (its bytes, `jmp $+2` loop) beside an armed image | 769 exits, `dac_batched=0` (old burst guard) | 1 exit, `dac_batched=768`, image and TF untouched, DAC exact (budget: at most 3) |
| lean PM CLI/STI against the scalar control | - | same virtual IF, STI shadow, its end at the next #GP, armed PUSHF image, IRQ delivered after the shadow, missed proof falls back with the seed demand on |
| OPL3 through 220h-223h | ordinary | lean, same register file as the scalar control |
| `PUSHF; CLI` (fx-16 site) | armed | armed (rule 1 does not apply) |

Updated expectations (behaviour change, with the reason in each test):
`test_fx27_lean` (a PM STI shadow ends at the #GP and the exit is lean),
`dosvm_cost_test.inc` and `dosvm_exit_test.inc` burst guards (an armed image
no longer stops the I/O burst; the burst retires the same instructions as
without one), `dosvm_speed_test.inc` (one more disk row, the presenter row).

`dosvm_exit_bench` (this host, AMD Ryzen 7 7800X3D, `nice -n 10`, TSC
cycles per exit, three alternating runs of `main` sources with
`-DT23_HEAD_BASELINE` and of this branch):

| case | `main` | this branch |
| --- | ---: | ---: |
| `pm_cli_war2_event_loop` (the candidate CLI) | 354,096-354,910 (missed every time) | 241-245 |
| `pm_cli_sti_settled_lean` | 795-808 (no lean path) | 237-243 |
| `pm_cli_sti_settled_ordinary` | 772-779 | 734-768 |
| reference: `pm_vga_byte_exit_settled_hot_lean` / `_ordinary` | 420-426 / 1,049-1,054 | 429-444 / 1,038-1,087 |

The bench's 354,000 cycles are its worst case (two 4,096-byte forward
decodes over a zero-filled code page); the trial's averages for Warcraft II
were 20.0 us per CLI/STI exit with misses (n=4) and 112.3 us (n=7).

### 5.4 Suites

Sequential, each under `systemd-run --user --scope -p MemoryMax=3G
-p MemorySwapMax=1G`, `nice -n 10`, on this branch:

| suite | result |
| --- | --- |
| `dosvm_test` (ASan/UBSan, `host_kernel_tests.sh` DOS VM block) | PASS, 60,606,140 checks, max record 237 bytes |
| `CIUKI_VM_DISK_TRACE=1`, `CIUKI_VM_FX16=1`, `CIUKI_VM_FX27=1`, `CIUKI_VM_T23DOSPERF=1` | PASS (fx-27 622,983 checks; t23-dosperf 40,151) |
| game-launch `frame`, `exit`, `close` | PASS |
| `CIUKI_VM_AUDIO_COST=1` | PASS (single 6,466-6,562 ns, double 12,615-12,808 ns) |
| `dosvm_exit_bench`, `dosvm_present_bench` | PASS (numbers in 5.3) |
| `tests/host/dos/run_dos_extenders.sh` | PASS (DOS/4GW client, Warcraft II with and without EMS) |
| `run_dos_memory.sh`, `run_unified_file.sh`, `run_file_api.sh` | PASS |
| `irq_work_replay.py` | PASS; `DOS-audio-clock-per-VM max_us=250 budget_us=250` (unchanged, at the limit), `DOS-clock-turn max_us=258` of 1,000 |
| `tests/host/vm/run.sh`, `tests/host/audio/run.sh` | PASS |
| `test_vm_game_records.py`, `test_dos_titles.py` | PASS |
| `python3 scripts/build_kernel.py` (kernel only, no image) | PASS, FPU/SIMD audit passed, `VMM.ELF` 17,096,304 bytes |

Reproduce the replay: `DOSX_PERF=1 bash tests/host/dos/run_dos_extenders.sh`
adds a 30 s Warcraft II run under the fx-27 T23 time model with an -O2
runner (per-second `H61 PERF` lines in `build/host/dos/extenders/war2-perf.log`).
On this branch it printed `DOSX war2-perf intro per second: traps=32902
lean=26790 missed=0 unprovable=4343 dac=0` and the suite PASS line.

## 6. Risks

- Rule 4 changes a contract stop condition (F3-33: "owned IF ... proof
  MUST stop a burst"). The argument rests on the burst decoders: if a later
  change lets `cvx_execute` or the I/O burst execute POPF, IRET, POP or a
  stack store, the armed-image stop must come back.
- Lean CLI/STI skip the ordinary per-exit housekeeping as fx-27's lean port
  exits do (UART/IPX service, present sampling, heartbeat, reports run at
  the next ordinary exit, at most one host tick later).
- The T23 per-exit costs are measured averages; the model's absolute
  numbers are estimates. The exit counts and the lean/burst shares come
  from production code and are exact for the replayed instructions.
- The replay's instruction clock is ~5x slower than the T23; the game's
  polling loops run proportionally fewer times, so absolute rates differ
  from the trial while the per-iteration mix is the same code.
- Not addressed: DOOM's return-tracker checkpoints (9 %), Mode Y plane
  OUTs, the PM INT 21h reflection (25 us), and the presenter's copies.

## 7. What the lead must check (QEMU smoke, then T23)

Warcraft II demo in the DOS window, intro movie and title/menu, then DOOM:

- `[vm-audio] vm=1 cadence_clock ... cli_missed=` stays near 0 for
  Warcraft II (trial: 9,232 -> 25,770) and `seeds_skipped` grows.
- `vm_perf_remap ... lean=` close to the exit total in every windowed bank
  (trial: 0-3.4 % except n=6), and `dac=` about 768 per palette load
  (trial: 0 in every bank).
- `vm_perf_cost cause=pm_flags` below 10 us per exit and `cause=pit_pic`
  below 10 us (trial: 20.0 and 20.2 us); `perf_port 03c9` a few hundred
  exits per 5 s instead of 184,514; `perf_port 0220/0222` about 7 us.
- `vm_perf_share monitor_permille` for Warcraft II in the window below 250
  (trial: 341-647) and `guest_permille` above the trial's 68-124.
- New `vm_perf_presenter ... wait_avg_us/wait_max_us` for both games: if the
  average delay is several milliseconds while `samples` stays far below the
  present rate, the windowed presenter is starved by the focused VM's
  scheduling preference (next item for the scheduler, not done here).
- DOOM: unchanged correctness; `pm_flags` per exit below 10 us; `dac=`
  about 768 per palette change.
