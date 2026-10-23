# FX-16: DOOM stutter and crackle after fx-9

Directive: `docs/directives/t23-e831e685-trial-fixes.md` (fx-16).
Written 2026-10-23 by a Claude agent in `../CiukiOS-wt-fx-16` (from
`e5d40038`). Evidence: T23 trial of preview `e831e685` (source `2cfed3a8`,
private, `legacy/local/physical/e831e685/`): `serial.log` (cited `Lnnnn`) and
`disk/LOGS/BOOT.LOG` (cited `B:nnnn`). Host evidence only; no QEMU, no image.

## Reference model (read before changing code)

- Win95 DDK [`VPICD_Set_Int_Request`](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S34C8.HTM):
  the interrupt is not simulated while "the virtual machine has interrupts
  disabled", "has masked the IRQ", "a higher priority virtual IRQ is in
  service" or "is suspended, or not able to run"; the virtual PIC is
  level-triggered (a request stays until cleared).
- Win95 DDK [`Wait_Semaphore`](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S33B8.HTM):
  `Block_Svc_Ints` "Service events and simulated interrupts in the virtual
  machine even if the virtual machine is blocked": a VM blocked inside a VxD
  service (for example a file read) still runs its interrupt handlers.
  [`Call_When_VM_Ints_Enabled`](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S33C6.HTM)
  and [`Adjust_Exec_Priority`](https://techshelps.github.io/MSDN/WIN95DDK/HTML/S3340.HTM)
  (VPICD uses `Time_Critical_Boost`).
- R. Chen, [Getting MS-DOS games to run on Windows 95: The interrupt flag](https://devblogs.microsoft.com/oldnewthing/20160411-00/?p=93281):
  Windows trapped CLI, "inspects the code before the cli for the pushf
  pattern or a known variant", checked that the top of the stack matches the
  VM's flags, and copied IF into the pushed TF so the POPF trapped one
  instruction later. This concerns Windows 95 DOS sessions, not NT NTVDM.
- Linux KVM i8254 (`arch/x86/kvm/i8254.c`, read 2026-10-23): reinject mode
  is the default; each expiry increments `pending`, the ack notifier
  (`kvm_pit_ack_irq`) injects the next pending tick, reprogramming resets
  `pending` (`kvm_pit_reset_reinject`).
- DOSBox Staging (`src/hardware/pic.cpp`, `src/hardware/timer.cpp`, read
  2026-10-23): PIT events are queued in emulated time (`PIC_AddEvent(PIT0_Event,
  delay)`), the CPU runs a fixed cycle budget per emulated millisecond and
  `PIC_runIRQs` raises a pending IRQ at the next boundary with IF set; no tick
  is ever late in emulated time, and POPF at CPL > IOPL leaves IF unchanged
  (`CPU_POPF`) exactly as on hardware.
- id Software [linuxdoom-1.10 `w_wad.c`](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/w_wad.c)
  `W_ReadLump`: `lseek` then `read` per lump, in the order the game needs
  them. Linux [`fs/fat/cache.c`](https://github.com/torvalds/linux/blob/master/fs/fat/cache.c)
  keeps contiguous runs per inode (`fat_cache_lookup`, `nr_contig`).

## What the trial shows

DOOM in a window from L4468 (`launch-stage ... window=DOOM`), full screen
from L5361 (`fullscreen_request`, `vm-park` tick 128852, `first-guest-frame`
tick 129777), back at L6151 (`watchdog ... cause=hotkey`, tick 151687).

| window (5 s) | record | reading |
| --- | --- | --- |
| W1 window, startup | L5292-5293 `gap_max_ms=9 dsp_lag_max_ms=9`, `late_acks=20 merged=66 ack_max_ms=179 irq_latency_max_us=178378 avg=3408 descheduled=382 if_clear=605` | DOOM loading: `perf_cost cause=rm_int exits=574 us=1563082` (L4975), `2344 us=1051349` (L5065): 1.0-1.6 s of every 2 s in synchronous monitor services |
| W2 switch | L5508-5509 `late_acks=11 merged=102 ack_max_ms=809 underruns=94 parked=808` | the VM parked 925 ms; its DSP and PIT kept running |
| W3 full screen | L5649-5650 `late_acks=15 merged=41 ack_max_ms=69 avg=2450 descheduled=146 if_clear=487` | `cause=rm_int exits=315 us=300135` (L5648), `exits=237 us=332000` (L5507); bank n=6: INT FEh 619 calls 736 ms (B:3892), IRQ pending with virtual IF clear +1033 ms (`irq_ms` 4782 to 5815, B:3862) |
| W4 full screen | L5859-5860 `late_acks=0 merged=0 ack_max_ms=11 avg=876 if_clear=6` | clean; INT FEh 100 ms (bank n=7) |
| W5 full screen | L6000-6001 `late_acks=3 merged=9 ack_max_ms=71 avg=1115 if_clear=96` | |

Full-screen CPU (BOOT.LOG banks n=6-9, B:3864-B:4087; `vm_perf_cpu` lines B:3864, B:3936, B:4010, B:4079): guest 25-37 %, monitor
25-44 %, off the CPU 30 %. Monitor per 5 s in the steady windows n=7/n=8:

| cause | exits | ms | us/exit |
| --- | ---: | ---: | ---: |
| I/O (OPL 388h/389h about 19,000, PIC) | 21,383 / 21,822 | 462 / 472 | 21.6 |
| PM CLI/STI (`pm_flags`) | 7,568 / 7,236 | 421 / 416 | 57 |
| `#DB` (return-tracker checkpoints 6,719 / 6,536) | 7,036 / 6,916 | 256 / 266 | 37 |
| host timer IRQ in the guest | 2,848 / 2,836 | 173 / 172 | 61 |
| PIC/PIT ports | 4,840 / 4,813 | 93 / 92 | 19 |
| INT FEh services (`rm_int`) | 344 / 225 | 105 / 83 | 300-370 |

Off the CPU (serial `[cpu] top`, L5639-L6124): `desktop#34` 65-68 permille,
`audio-refill` 58-75, `device-work` 50-71, `vm-clock` 40; `stretch_max_ms=7
task=desktop#40` and `=8 task=dosvm#59`, both `eip=00000000` (L5641, L5991,
L6126), matching `cadence_clock gap_max_ms` 8-9. In a window (bank n=4,
B:3723-B:3752): guest 13 %, monitor 55 % (`vga_pf` 19,968 exits 491 ms,
B:3739), 26 flips in 5 s (B:3725).

## Root causes

1. **Synchronous VMM file services with no interrupt service** (W1 178 ms,
   W3 69 ms). A DOS read handled in the monitor runs the guest not at all.
   Windows 98 keeps simulating interrupts into a VM blocked in such a service
   (`Block_Svc_Ints`); CiukiOS does not. What made the services long: FAT
   reads seek through `file_cluster`, which, like MS-DOS `FNDCLUS`, restarts
   from the first cluster when the target lies before the cursor. DOOM's
   lump reads are scattered, so half of them walked the DOOM.WAD chain:
   2,204 FAT lookups per backward lump read on the host fixture, about
   2.2 ms each on the T23, consistent with INT FEh's 0.3-2.7 ms per call.
2. **Virtual IF left clear inside DMX's timer service.** DOOM/DMX's service
   (`push ebx; push edx; pushfd; pop eax; cli` at 01df:00244264 in the host
   DOOM replay) is reached from its dispatcher after `EOI; sti; ...
   lss esp`. No forward decode crosses the stack switch, so the CLI was never
   armed (1,134 misses in the replay); DMX's POPFD then left virtual IF clear
   for the rest of the timer handler, where IF recovery is excluded by
   design, and the SB16 IRQ that DMX had re-enabled waited for the handler's
   return. On the T23 the handler's OPL writes cost 22 us each, so these
   waits are about 1 ms each, plus up to 2 ms forced recoveries in ordinary
   code.
3. **The monitor spends as much CPU as the game** (full screen 25-44 %). Of
   that, 0.68 s per 5 s exists only to keep forward-decode seeds fresh for
   the CLI proof: the return tracker's checkpoint `#DB` exits, the 4,096-byte
   decode plus the 4,096-step control-flow search on every candidate CLI
   (57 us per CLI/STI exit), the boundary decode on every physical IRQ in PM
   and a 16-instruction seed on every PM trap. DOOM/DMX run the same handful
   of flags-saving CLI sites thousands of times per second.
4. **Lost DMX/DOOM timer ticks.** The 8259A latches one request: an edge
   that finds IRQ0 still requested, or a second edge in one late clock turn,
   vanished. With `gap_max_ms` 8-9 against DMX's 7.14 ms period and 7-8 ms
   kernel stretches, the replay of those gaps loses 31 of 704 ticks in 5 s
   (4.4 %, 8 DOOM tics): game time and music slip and then jump.
5. **The full-screen switch kept the parked VM's devices running.** For
   808 ms the DSP replayed the unrefilled auto-init buffer (a buzz) and every
   block merged into one latched IRQ (`ack_max_ms=809`).
6. **Not fixed here (measured, see report):** the desktop's 6.5 % CPU and
   7 ms non-preemptible stretches during a full-screen game, `vm-clock`'s
   1 kHz context switches (4 %), the generic 20 us exit cost (OPL I/O 8 % of
   the CPU), windowed VGA emulation (45 % of the CPU in the window model),
   and the synchronous file-service model itself.

## Decisions and changes

- FAT leading-run cache (`fat.c read_cluster`, `vfs-storage-contract.md`
  fx-16 amendment).
- CLI proof cache, Windows 95 pattern rule after the forward proofs, no
  recovery steps while an image is armed (`execution-abi.md`, fx-16
  amendment to F3-44); boundary seeds on demand (`dos-dpmi-contract.md`,
  fx-16 amendment).
- Paced IRQ0 reinjection and frozen device time for a parked VM
  (`execution-abi.md`, amendment fx-16).
- Diagnostics: kernel stretch EIP sampled from 4 ms; the protected
  `[vm-audio] ... cadence_clock` line adds `irq0_reinjected`, `irq0_lost`,
  `irq0_owed_max`, `parked_ms`, `cli_hits`, `cli_learned`, `cli_missed`,
  `seeds_skipped` (lifetime per VM generation).

## Host evidence

| case | before (e5d40038) | after |
| --- | --- | --- |
| DMX service CLI behind a stack switch (`CIUKI_VM_FX16=dmx`) | not armed; FAIL | armed, POPFD restores IF, IRQ7 nests in the timer handler |
| 140 Hz PIT with 9 ms late services and 8 ms stretches (`pit`) | 673 of 704 ticks; FAIL | 704 of 704, 31 reinjected, owed at most 1 |
| 808 ms park, DMX SB setup (`park`) | 70 stale blocks, 69 merged, ack age 807 ms; FAIL | 0, 0, at most 9 ms |
| DOOM lump reads, 6,674-cluster file (`doom_lump_reads`) | 2,204 lookups per backward read; FAIL | 6.2 |
| Real DOOM, shooting 5 s, host instruction clock | SB16 latency max 9.8 ms, 10,787 `#DB`, 20,432 recovery steps, 4 forced restores | 1.0 ms, 4,649, 300, 0 |
| Real DOOM, T23 time model, full screen | monitor 43.5 %, IF clear 564 ms, 588 M guest instructions; replay target FAIL | 30.4 %, 216 ms, 759 M; PASS |

The T23 time model (`dosvm_guest.c`, `GUEST_T23=1`, documented there)
charges each exit this host's measured monitor time scaled per cause to the
trial's T23 averages, and adds the trial's off-CPU pattern. Its HEAD run
matches the trial's banks (CLI/STI 414 ms per 5 s against 391-421 ms, I/O
451 against 359-504). It is a model, not hardware evidence.

## What the next T23 trial must show

- `[vm-audio] cadence` in full screen: `late_acks=0`, `merged` under 1 % of
  `blocks`, `if_clear` under 50 per window, also while DOOM loads sounds;
  `cadence_clock ... cli_missed` stable, `seeds_skipped` growing,
  `irq0_lost=0`.
- `vm_perf_cost` per 5 s full screen: `pm_flags` under 150 ms, `db` under
  1,500 exits, monitor share under 30 %; `vm_perf_vector` INT FEh under
  0.5 ms per call.
- Full-screen switch: `parked` > 0 with `merged` and `late_acks` 0 in that
  window and no buzz; `cadence_clock parked_ms` about the switch time.
- `[kernel] stretch_max_ms` lines now name an EIP for 4-20 ms stretches: the
  7-8 ms desktop/VM stretches must be attributed (map with `VMM.ELF`).
