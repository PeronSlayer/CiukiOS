# fx-27: windowed DOS graphics (Quake and DOOM in a window)

- Written 2026-10-23 23:41 CEST, completed 2026-10-24 04:20 CEST, in `CiukiOS-wt-fx-27` (branch `wt/fx-27`,
  from `6b0a5481`). Directive: `docs/directives/t23-f441c5bd-trial-fixes.md`
  (fx-27). Design: `docs/design/f3-acceptance.md` ("fx-27"),
  `docs/design/dos-dpmi-contract.md` ("fx-27 amendment").
- Host evidence only: no QEMU, image build or hardware run. Every T23 figure
  below is either a T23 trial record or a stated model; the next T23 trial
  measures the real result (last section).

## 1. What the trials show

Trial f441c5bd, Quake 1.06 in a window (mode 13h), BOOT.LOG banks of 5 s:

| bank | frames | DAC 3C9h exits | PIT 40h/43h exits | DMA 00C6h/00D8h | present | harvest | monitor | off-CPU |
| --- | ---: | --- | --- | --- | ---: | ---: | ---: | ---: |
| n=3 | 31 | 17,664 / 386 ms (21.8 us) | 12,909 / 293 ms (22.7 us) | 2,922 / 74 ms | 174 ms | 33 ms | 41.8 % | 42.4 % |
| n=4 | 11 | 768 / 16 ms | 26,536 / 666 ms | 2,328 / 61 ms | 45 ms | 14 ms | 36.3 % | 40.4 % |

Per Quake frame (n=3): 570 DAC OUTs (0.74 palette loads of 768 bytes),
416 PIT exits (`Sys_FloatTime` calls `dos_outportb(43h,0)` and
`dos_inportb(40h)` twice, through CALL/RET, so the I/O burst cannot join
them), 94 DMA position polls, 16 chain-4 pages deinterleaved, 291 surface
rows. The serial log (`[dosvm] perf_port`) shows the same split per 2 s.

DOOM 1.9 in a window: trials a79f840c (f3-38: 103,432 VGA exits / 1,440 ms
per 5 s, 16 frames) and e831e685 (bank n=4: 21 frames, `vga_pf` 19,968 at
24.6 us during the start-up, then the per-column map-mask OUTs). The fx-16
T23 model attributes 44 % of the CPU to VGA port exits at about 5 fps.

Every such exit costs 14–22 us on the T23, against 11–12 us for a trivial V86
exit (f441c5bd n=3: `rm_cli` 11.9 us, `rm_sti` 12.2 us). Agner Fog's tables
put CPUID at about 130 and RDTSC at about 42 cycles on the T23's P6 lineage,
so the monitor's three serialized clock reads per exit are about 0.8 us; the
rest is the monitor's per-exit work and its cache misses.

## 2. Research

Windows 3.0/95 VDD (*Virtual Device Adaptation Guide*, Windows 3.0 DDK,
bitsavers `Virtual_Device_Adaptation_Guide_Version_3.0_19900401.pdf`):
18.1.2 "When an application is running in the background, the VDD traps all
the video I/O, saving the output port values and emulating the input port
values"; 18.4.1 "Get_Mod returns a data structure which indicates
modifications to the current display. The Grabber DLL will modify only those
parts of the window that have changed and then issue a Clear_Mod"; chapter 19
`ModifyPageBits` "always has the side effect of clearing P_DIRTY and P_ACC".
Windowed DOS video is trap-and-emulate for ports and dirty-tracked memory with
incremental repaint; Raymond Chen ("what is a grabber", 2023) notes it was
slow on the hardware of the time. CiukiOS keeps the model and makes the
trapped write and the repaint cheap.

DOSBox 0.74 (`src/hardware/vga_memory.cpp` `VGA_ChainedVGA_Handler`
`writeCache`: chained stores also go to the linear `fastmem` copy the
renderer draws from; `src/gui/render.cpp` `RENDER_StartLineHandler` compares
every line with the previous frame's cache and reports `Scaler_ChangedLines`
to `GFX_EndUpdate`; `RENDER_SetPal` keeps the first/last changed entry and
`Check_Palette` applies them once per frame, then a full update). QEMU 10.1
`hw/display/vga.c` `vga_draw_graphic` (dirty-page snapshot per scanline,
`update_palette256` forces a full update, `dpy_gfx_update` per changed run).
Linux `arch/x86/mm/tlb.c` `tlb_single_page_flush_ceiling = 33` and
`Documentation/arch/x86/tlb.rst` (INVLPG per page below the ceiling: "the
cycles you spend doing invlpg" against "the cycles that you spend refilling
the TLB later"); Intel SDM Vol. 3A 4.10.4.1 (MOV CR3 flushes all non-global
entries; CiukiOS kernel PTEs are not global). Quake 1.06 `QUAKE.EXE`
disassembled for the palette and PIT code (instruction forms reproduced in
`tests/host/dosvm_fx27_test.inc`; no game bytes copied).

## 3. Changes

See the design amendment. In short: lean exits for hot port I/O; per-entry
INVLPG instead of CR3 reloads; one exit per palette load (DAC burst); chain-4
planes deinterleaved only for model consumers and chained rows presented from
the linear backing; index-proven rows written without surface read-back; VBE
rows only from dirty pages (DDA columns, LUT); `vm_perf_remap lean= dac=
linear=`.

## 4. Budgets (deterministic, `dosvm_test`, `CIUKI_VM_FX27=1`)

`tests/host/dosvm_fx27_test.inc`; the baseline column is the same file built
with `-DFX27_HEAD_BASELINE` against `6b0a5481` sources (non-fatal budgets).

| budget | limit | 6b0a5481 | fx-27 |
| --- | ---: | ---: | ---: |
| Quake `VGA_SetPalette` (PM32): exits per palette load | 2 | 769 FAIL | 1 |
| DOOM-style `I_SetPalette` (V86): exits per palette load | 2 | 769 FAIL | 1 |
| DOOM frame, 1,279 map-mask OUTs: CR3 reloads | 0 | 1,278 FAIL | 0 |
| same: per-exit housekeeping (`kpreempt_point` calls) | 0 | 1,279 FAIL | 0 |
| Quake 13h frame: chain-4 pages deinterleaved | 0 | 16 FAIL | 0 |
| changed row: surface read-backs | 0 | 2 FAIL | 0 |
| VESA 640x480x8, 32 lines written: rows converted | 40 | 480 FAIL | 32 |

Correctness in the same file: scalar and burst palettes identical; burst
stops before a store, a VGA memory load and a deliverable IRQ (which is then
delivered); lean exits refused for TF, IPX, idle polls, seed demand,
shadows, an interrupted context's return point, DMA pinning and pending IRQs;
Quake's PIT latch/two reads return the same values lean and scalar; chained
presentation from the backing equals the reference presenter on the planes
(start address, pan) and the planes are exact after a model consumer; VBE
partial presentation equals a full redraw (640x480x8, 800x600x16).

## 5. Host measurements

AMD Ryzen 7 7800X3D, `nice -n 19`, O2 without sanitizers
(`dosvm_present_bench_plain`, `FX27_BENCH=1`), three alternating runs each,
ranges:

| pattern | 6b0a5481 | fx-27 |
| --- | --- | --- |
| Quake palette load (5,376 instructions): exits / host us | 768 / 497–502 | 1 / 239–240 |
| Quake 13h frame present: host us / deinterleaved pages | 91.2–92.9 / 16 | 50.9–52.4 / 0 |
| VESA 640x480x8 full frame present: host us | 506–511 | 104–107 |
| VESA 640x480x8 status bar (32 lines): host us / rows | 506–510 / 480 | 7.8 / 32 |

(The palette figure includes the test's decoder running the instructions
between exits for 6b0a5481, as libx86emu does in the guest runner.)

Exit cost (`dosvm_exit_bench`, TSC cycles, settled DPMI client, same VM, the
ordinary path forced only by the IPX-binding precondition; cold = 4 MiB
walked before each exit): hot lean 415–472 against ordinary 912–988 cycles
(0.45–0.48); cold lean 664–1,535 against 1,295–3,029 (0.45–0.67).
`rm_vga_byte_exit` 326–357 → 200–216 ns, `rm_pic_eoi_exit` 244–256 → 95–104
ns, `rm_pit_latch_two_reads` 427–476 → 263–296 ns (same machine load,
alternating binaries).

## 6. Per-frame cost on the T23 model

**Quake mode 13h (from the f441c5bd n=3 frame above; per-exit costs are the
trial's).** Before: DAC 570 × 21.8 us = 12.4 ms; PIT 416 × 22.7 = 9.4 ms;
DMA 94 × 25.3 = 2.4 ms; harvest 1.05 ms; present 5.6 ms: **30.9 ms of VGA
and hot-port monitor work per frame** (the game's own rendering, INT FEh
loading and the desktop are extra). After: DAC 0.74 loads × (one exit +
5,376 decoded instructions at 45 ns host, about 0.5 us on the T23 at a 12x
compute ratio) ≈ 2.1 ms. For PIT and DMA I assume DOOM's measured 68–100 %
lean share at 0.45–0.67 of the ordinary cost; no Quake gameplay replay
exists in the runner, so Quake's own share is not measured. That gives PIT
≈ 4.2–7.3 ms and DMA ≈ 1.1–1.9 ms. Harvest falls to 0. Present falls to
≈ 4.5–5 ms: there is no read-back and rows come from the linear backing,
but the 1 MB surface write per frame remains. Total: **≈ 12–16 ms per
frame**. The same palette, PIT and DMA gains apply to Quake's mode X modes,
≈ 13–17 ms; their plane copies were already direct, with 4 map-mask OUTs
per frame.

**Quake VESA (640x480x8 LFB).** Before: any dirty page converted all 480
rows with two divisions per pixel (614,400 divisions, about 22 ms of P6
divider time per frame) and a DAC expansion per pixel. After: rows of dirty
pages only, LUT per pixel, no division: the host full-frame cost falls 4.8x,
a 32-line update 65x.

**DOOM Mode Y (guest runner, real DOOM 1.9 shooting replay, 5 s).** The
runner now honours the window's PTEs (aperture stores reach the mapped plane
with A/D bits, otherwise #PF), so the replay contains the real direct-window
behaviour. Before: about 953 VGA exits per frame at the T23's 19.7 us =
**18.8 ms per frame**, plus 209 OPL exits (4.5 ms). After: 68–85 % of the
port exits of the gameplay perf bank are lean (`vm_perf_remap lean=` over
the `vga_io`, `pit_pic` and `io` exits of that bank, five runs). The rest are
CLI-seed demand windows, V86 exits under DPMI, and exits with a pending IRQ.
At the measured lean/ordinary ratio of 0.45–0.67, that gives **≈ 10–15 ms
per frame** for VGA exits and ≈ 2.6–3.5 ms for OPL. The per-OUT exit itself
stays, because a different plane backs the same addresses.
Model runs (`tests/host/dosvm_guest.c`):

| model | 6b0a5481 frames / monitor ‰ / vga_io us | fx-27 frames / monitor ‰ / vga_io us |
| --- | --- | --- |
| fx-16 ratio (`GUEST_T23=1`) | 117–121 / 650–655 / 19.6–19.8 | 124–128 / 618–624 / 17.1 |
| fx-27 two-point (`GUEST_T23_MODEL=fx27`), slices 199/200/201 k | 108 / 665–669 / 19.7 | 132–147 / 353–589 / 4.5–13.8 |

Why two models. The fx-16 model multiplies the whole host exit time, and on
this host about 1.3 of HEAD's 1.86 us per VGA exit are the monitor's own
`clock_gettime` calls (424–446 ns each), which cost the T23 about 0.27 us;
it cannot see a code-path change. The two-point model charges ordinary
fixed-path exits the T23's measured average and scales lean exits by their
host code time (clock calls removed) over HEAD's; that code time varies 7x
with this host's cache state between runs (51–372 ns), hence the range. The
cold/hot bench ratio (0.45–0.67) is the better estimate of the per-exit gain.
In both window models HEAD itself misses the fx-16 full-screen ISR target
(virtual IF clear 262–340 ms per 5 s in the two-point model); the faster
build spends more host ticks in guest mode, each charged as a 60.7 us timer
exit, and shows 285–537 ms. That target belongs to full-screen play.

Two early two-point runs of fx-27 stopped DOOM after its first page flip.
From then on they took only host-timer exits, with no IRQ deliveries and no
port exits. One run had lean exits on at slice 203,000; the other had them
off (`GUEST_NO_LEAN=1`) at slice 200,000. Both stopped at the same guest
point, with identical aperture counters. `6b0a5481` passed at both slices.

Three reruns used an end-state dump and ran on a busier host: lean exits
on at slice 203,000, and off at slices 203,000 and 200,000
(`stall-diag*.log`). None stopped. All three ended
in DOOM's column loop with virtual IF set, no armed image, no open record
and only IRQ1 pending.

The model charges host-measured times, so its runs are not deterministic.
The cause of these stops is not established. Since the stop occurs with
lean exits off as well, it does not come from the lean path. Whether
another fx-27 change (DAC burst, lazy planes, INVLPG) or only the changed
exit timing triggers it is still open. The DEMO1
timedemo runs on the instruction clock without a model and completes on
both fx-27 runners with identical counts. Before a trial, a quiet-host
rerun of the two-point model at slices 200,000 and 203,000 should either
reproduce the stop with the end-state dump or show that it does not occur.

### 6a. Armed flags images in DOOM's IRQ dispatcher (2026-10-24)

The f3-38 DEMO1 timedemo (`-nosound -timedemo demo1`, no T23 model,
`build/codex/fx-27-logs/doom_timedemo.sh`) passed on the first fx-27
runner (5,026 gametics in 43,347 realtics, prompt back) but took only 7
lean exits: 9.55 M port exits were refused because an fx-16 flags image
was armed. A diagnostic run over the first 1.0 G instructions attributes
1.53 M of the 1.56 M refusals to the CLI at 01df:002465b2 and 30 k to the
CLI at 01df:002464e8. Both are in DOOM's protected-mode IRQ dispatcher:
it pushes registers, executes CLI, then either chains to the previous
handler with PUSHFD and a far CALL or sends EOI to A0h/20h itself, pops and returns. Only 64 new
arms happened over that stretch, and the run took 5 flags #DB traps in
total. The patched image is therefore almost never loaded, and `if_armed`
stays set across frames. At `6b0a5481` the same state already disabled the
I/O and video bursts and IF-recovery stepping.

The ordinary path does nothing for an armed image that has not been
loaded, at an I/O exit without TF and without a pending IRQ:

- `flags_restored` needs the step;
- `return_boundary_seed` returns before any seed or count;
- the bursts refuse it themselves;
- `if_recovery_prepare` only ends recovery.

So fx-27 now lets such exits be lean (`dpmi_lean_eligible`; the skipped-seed
count follows `return_boundary_seed`). `test_fx27_lean` checks both virtual
IF states against the ordinary control; it fails on the earlier fx-27 code.

On the timedemo, lean exits went from 7 to 9,577,450, about 98 % of the
9.37 M VGA port exits. The guest-visible result is unchanged: both runners
retire exactly 5,107,554,352 instructions and 13,832,983 traps, and DOOM
prints the same `timed 5026 gametics in 43347 realtics`. The runner's clock
counts instructions, so equal counts mean equal guest execution. Host time
per exit is not comparable between the two runs, because the second one
shared the host with the suite chain. Whether those CLI
sites should arm at all (the fx-16 proof of their PUSHF image) is left to
fx-16. A stale armed image also keeps the I/O bursts and IF-recovery
stepping off.

## 7. What the next T23 trial must show

Windowed Quake (13h) and DOOM, BOOT.LOG `vm_perf_*` banks:

- `vm_perf_remap lean=` close to the VGA/PIT/OPL exit counts in gameplay
  (DOOM: above 80 % of `vm_perf_cost cause=vga_io`);
- `vm_perf_cost cause=vga_io us/exit` well below 19.7 (target ≤ 12);
  `pit_pic` below 19 us; `perf_port 03c9` exits about one per palette load
  and `dac=` about 768 per load;
- `vm_perf_present harvest_us` near 0 for mode 13h, `present_us` per frame
  below 5.6 ms; `vm_perf_remap linear=` equal to the converted chained rows;
- more `vm_perf_video frames` per bank than f441c5bd n=3 (31) and n=4 (11).

## 8. Commands and results

All from the worktree root with `nice -n 19`; scripts in
`build/codex/fx-27-logs/` (`build_vm.sh` builds exactly the
`host_kernel_tests.sh` dosvm slices, plus `benchplain`).

| command | result |
| --- | --- |
| `build_vm.sh $PWD build/host test` then `build/host/dosvm_test` | PASS (59,808,476 checks, max record 237 bytes) |
| `CIUKI_VM_DISK_TRACE=1 build/host/dosvm_test` | PASS |
| `CIUKI_VM_FX27=1 build/host/dosvm_test` | PASS (7 budgets, 622,980 checks) |
| same, `-DFX27_HEAD_BASELINE` on `6b0a5481` sources | 7 budgets FAIL (table 4) |
| armed-image lean case against the code before 6a | FAIL (`test_fx27_lean`), PASS after |
| `bash tests/host/vm/run.sh`, `bash tests/host/audio/run.sh` | PASS |
| `test_kernel_latency.py`, `test_kernel_stack.py`, `test_system_stalls.py`, `record_scope_test.py` | PASS |
| `irq_work_replay.py` | PASS at normal priority. At `nice -n 19` with load average 13–14 from other worktrees' jobs, its audio resampler host-CPU budget (`pipeline_test.inc:117`) failed; fx-27 changes no audio code |
| `test_vm_game_records.py`, `test_dos_titles.py`, `tests/host/dos/run_file_api.sh` | PASS |
| ledger (normal/strict), reclaim, mtrr, kernel_pde, `test_virtual_vga`, `dosvm_guest_step_test`, `dosvm_exec_test.py` (Quake EXEC/CRT alias) | PASS |
| `dosvm_exit_bench`, `dosvm_present_bench` | PASS (numbers above) |
| `python3 scripts/build_kernel.py` | PASS, FPU/SIMD audit passed; `VMM.ELF` 16,095,364 bytes, SHA-256 `a2b8cf6223beb7476b6bf7c8ae1f45d398aa5a4a4c41fb2736f32fc4c4a01871` (host build only, no image) |
| DOOM DEMO1 timedemo (`doom_timedemo.sh`), both fx-27 runners | PASS: 5,026 gametics in 43,347 realtics, prompt back, identical instruction and trap counts |
| DOOM shooting replay, both models, both sources | as table 6. On the fx-16 ratio model the runner passes for `6b0a5481` and fx-27 on a quiet host. On the two-point model it exits 1 on the fx-16 full-screen ISR target for both (section 6). The final reruns after 6a (`final3-*.log`) ran at load average 13–14, so their host-scaled times are not comparable and they fail that target under both models. 6a only adds lean exits; in these replays fewer than 3 % of the exits were refused for an armed image |

Fixtures that some suites need and the worktree lacked were generated
first: `build/f0/files-payload.elf` (by the kernel build) and
`build/host/optical/fixture-0.iso`
(`scripts/test/iso_fixtures.py --out build/host/optical --joliet`).
