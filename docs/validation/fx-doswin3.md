# DOS window cost investigation

Research checked 2026-10-26. Directive: `docs/directives/fx-doswin3.md`.
Host evidence only; the physical measurements refer to image `5bb73dba`.

## Sources and implementation decision

- The Windows 98 Resource Kit, local read-only `wrkc24_3.htm`, lines
  202–214, assigns memory drawing and unsupported screen operations to the
  DIB engine. `wrkc25_3.htm`, line 330, describes automatic DOS foreground,
  background and video-port policy. This is the Windows 9x VDD model.
- [FreeVGA sequencer registers](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/seqreg.htm)
  defines the independent plane write enables. A multi-plane store cannot
  be replaced by a native write to one plane followed by page comparison:
  an equal-value store must still overwrite the other enabled planes.
- [Wolfenstein's original scaler generator](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/WL_SCALE.C),
  `BuildCompScale`, emits RAM loads and ES-relative aperture stores. The
  current host replay already reproduces those instructions. The monitor
  executes every instruction twice, preflight then commit, and currently
  repeats RAM translation and protected-segment validation per byte.
- [X.Org Savage XAA](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_xaa.c)
  exposes ordinary copies, fills, image writes and monochrome expansion.
  Its colour expansion selects foreground/background, not a 256-entry RGB
  lookup. This does not establish an indexed-8 to XRGB32 stretch operation
  for the existing BCI transport. Keep capability reporting and the generic
  display interface unchanged; do not invent an accelerated palette hook.
- [QEMU VGA](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c),
  `update_palette256` and `vga_draw_graphic`, samples palette/dirty state at
  refresh. CiukiOS already shares a cadence between WAIT and PRESENT; a VGA
  write does not force conversion. Preserve this and exact row damage.
- [Intel's 80386 instruction reference, DEC](https://pdos.csail.mit.edu/6.828/2018/readings/i386/DEC.htm)
  specifies the flags and unchanged carry. The counted DAC loop's DEC
  flags are overwritten by INC before its next OUT; INC still goes through
  the existing instruction decoder. Recognize the live nine-byte loop,
  validate every resident source byte, preserve the original six-instruction
  lookahead bound per OUT and leave the final DEC/JNZ native. The complete
  register/flag result is compared with the general decoder in host tests.

Optimize the existing bounded VGA instruction burst using the same resident
RAM-page and descriptor cache already used by DAC bursts. Keep byte values
live, retain preflight before side effects, and never retain a cache across
a scheduling point or a mapping-changing VGA port. The committed replay of
latch/port/string operations clears RAM aliases and uses the original
resolver, since existing bulk/VBE transfer paths can yield. Keep native full-screen
leases, scheduling policy, IRQ delivery, unsupported-instruction fallback
and display-driver capability selection unchanged.

The scalar-store preflight retains up to four validated output bytes. With
no latch read, port or bulk-string side effect, commit those bytes in order
and reuse its register result. Other operations retain both passes. A
refused instruction commits no bytes. The counted DAC loop is admitted only
in windowed 32-bit code, with EBX/ESI/EDI sources; unsupported patterns keep
the general decoder. Neither optimization bypasses a debug step, shadow,
pending guest IRQ, reschedule, source fault or instruction budget.

`vm_perf_checkpoint` gains `dac_counted`, the interval's number of DAC bytes
handled by the counted-loop recognizer. It reuses this short existing row:
adding a row exceeded the four-VM stalled-drain record budget; extending the
already long remap row exceeded the 240-byte test record boundary. The final
change preserves the record count and tests both interval capture and reset.

The resumed API check rules out assuming that legacy COPY can already be
batched on the T23. `savage2d.c` installs `copy_rect` but no `command_bits`
or `command` hook; `fbdev_command_info` therefore reports no native COPY
command, and `batch_native` refuses it. The batch fallback would copy scanout
in software, introducing PCI reads. No compositor batching change is retained.
Connecting that transport requires a qualified command hook under the existing
display-command semantics, before assigning it a physical speedup.

## Physical cost reconstruction

The selected steady banks are Wolfenstein generation 11245 bank 6 (window)
and bank 9 (full screen), DOOM 12642/4 and /7, Warcraft II 15825/7 and /5.
All last 5,000 ms. Mode-grant records establish full-screen ownership; zero
virtual flips alone does not establish it. Wolf bank 7 spans a transition.

| Workload | Window guest / monitor / off-CPU us | Full-screen guest / monitor / off-CPU us |
| --- | --- | --- |
| Wolf | 550177 / 2315028 / 2134795 | 2125045 / 1424282 / 1450673 |
| DOOM | 1038023 / 1785788 / 2176189 | 2316048 / 1160758 / 1523194 |
| Warcraft II | 353965 / 2385749 / 2260286 | 1325520 / 1804821 / 1869659 |

Wolf's window costs 646,740 us in VGA I/O and 885,439 us in 3,799 costed
aperture faults. DOOM spends 907,307 us in VGA I/O (including 392,516 us of
remapping), 73,157 us in aperture faults and 377,355 us in return-tracker
debug exits. Warcraft's VGA I/O costs 1,771,541 us, almost exactly the sum
of its DAC index/data port costs; 346,155 batched writes show that the earlier
armed-image fix works. Its remaining counted-loop decoding is expensive.

The kernel presenter costs 176,473 / 102,724 / 231,547 us for 58 / 34 / 22
changed Wolf / DOOM / Warcraft frames. It writes 12,716 / 7,372 / 9,659
rows of 2,560 bytes. Viewport and compositor time comes on top. Presenter
dispatch maxima are 100,018 / 102,934 / 10,943 us. Changed frames, samples
and source flips are distinct counters.

The scheduler gives both focused modes the same preference; the additional
desktop work differs. Busy full-screen audio dispatches are capped at 2 ms,
versus up to 8 ms for a focused windowed VM; the focused-tick preference is
shared. A nearby Wolf window sample has VM 570 permille,
desktop 177 and DOS Box 46; a DOOM full-screen sample has VM 676 and desktop
51. These two-second samples do not align with every five-second game bank.
The supplied logs therefore do not establish per-game compositor time/frame.
No scheduling, IRQ/IF, timer or memory-limit policy was changed. The selected
window banks receive 5,000 VM clock services; DOOM lags with no lost IRQ0,
while Wolf's lost-edge counter also grows in the smooth full-screen interval.

Earlier 101-dosvid improvements address damage/copies, not these VGA exits.
101-dosvid2's direct LFB improves full screen, and its 3dfx STRETCH is not a
T23 capability. Earlier c42f556e Warcraft had 9,232–25,770 CLI misses and
zero DAC bursts; the new trial has zero misses and substantial DAC batching,
but the counted loop still consumes 1.77 s of monitor work per selected bank.

## Host evidence and model boundary

The same new replay cases were built against the base production VGA unit
(`be7852468`) and the changed unit, sharing the remaining host seams and
private telemetry layout. No old OS image was built. Host microseconds:

| Replay | Before | After |
| --- | ---: | ---: |
| DOOM Mode Y/frame | 1711.693 | 1711.029 |
| Wolf near/frame | 14901.156 | 5308.797 |
| Wolf near, fault CPU/frame | 14733.541 | 5155.890 |
| Wolf mid/frame | 24470.033 | 8537.349 |
| Wolf mid, fault CPU/frame | 24227.443 | 8313.392 |
| Warcraft counted palette/load | 937.550 | 166.931 |

Far/native-plane and latch-copy controls retain their behavior. DOOM's
1,279 plane-mask writes/frame are unchanged. All replay frame bytes are
checked. Host time is not a physical PIII measurement.

The scoped T23 replay uses physical `guest + monitor + kernel-present`
costs and applies the slower measured Wolf fault ratio (0.349942353) and
the palette ratio (0.178050237). It does not speed up DOOM speculatively.
With `C = guest + monitor - vga_io - vga_pf`, the same-trace native-video
reference, the before/after work ratios are DOOM 1.588/1.588, Wolf
2.282/1.850, Warcraft 3.069/1.565. The stated envelope is 1.9x for this
VM/presenter slice. Each baseline plus its separately retained off-CPU
remainder reproduces the captured 5,000,000 us bank exactly.

This does **not** qualify an end-to-end frame-time factor. Wolf's model
assumes its expensive PF time is scaler-dominated; at least 88.4% must
belong to the optimized class to meet that envelope. The physical opcode
mix and compositor/viewport/PCI prices are not present in these records.
The Warcraft estimate additionally assumes its measured palette work uses the
replayed counted-loop pattern; port counts alone cannot prove that. Compare
`dac_counted` on the next physical trial. The tracked tool
`tests/host/dosvm_window_cost_model.py BEFORE_LOG AFTER_LOG` reproduces the
calibration without private inputs, using the before/after production replay
logs named in the report.

The new production viewport/compositor replay checks nine title-shaped
Mode Y/VBE8 and zoom combinations against complete RAM/scanout oracles.
Unchanged frames issue zero display requests. A changed Mode Y Fit frame
still copies 965,120 viewport bytes, composes 241,280 pixels, uploads 512,000
bytes, and copies 113,280 pixels through 354 COPY/upload requests. VBE8 Fit
copies/uploads 1,157,120 bytes through one request. These stages are unchanged
by the monitor optimization and remain part of physical qualification.

The full DOS body passes 60,916,229 assertions (maximum record 237 bytes).
Default LeakSanitizer fails sandbox finalization; DOS, focused DOS, benchmark
and DOS Box properties process exits require lead reruns with leak detection
outside ptrace. Direct counted-loop guard tests also pass: a one-component budget and all
14 stop conditions preserve the required state. No sanitizer override was introduced. Under their existing
policies, display model (11), commands (3), presenter (4), Savage family (1),
desktop acceleration (5), desktop performance/replay (3), native desktop (1),
kernel latency (15) and instruction vectors (5) pass. DOS file/VFS suites and
four affected i686 production units also pass. No public ABI/SDK change,
vendor import, new panic path, image build or QEMU run.

Full commands, physical bank figures, source citations, calibration recipe,
limits and decisions are in `build/codex/fx-doswin3-report.md`. The lead must
decide whether the scoped 1.9x envelope is sufficient; the recurring gameplay
complaint remains unqualified. Next T23 evidence should compare VM CPU/cost/
port/remap/presenter banks, `vm_perf_checkpoint dac_counted`, desktop frame/
accel work, CPU shares and VM-audio/kernel latency against the image hash.

## Round 2 (2026-10-27): replay with the real games, presenter pacing, exit costs

Host evidence only, against the sources of round 1 (`a59c3eb70`). No QEMU,
image build or hardware run; physical figures are trial `5bb73dba`.

### Sources and decisions

- **Intel SDM Vol. 3A rev. 090** ([PDF](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf)),
  5.10.2.3 "Details of TLB Use": "the processor does not cache a
  translation for a page number unless the accessed flag is 1 in each of
  the paging-structure entries used during translation; before caching a
  translation, the processor sets any of these accessed flags that is not
  already 1", including translations for prefetches and speculative
  accesses; 5.8: the dirty flag is set by writes; 11.1.2.1 "Automatic
  Locking" (XCHG with a memory operand); "Serializing Instructions": INVLPG
  is a privileged serializing instruction.
  [FreeBSD i386 `pmap_enter`](https://raw.githubusercontent.com/freebsd/freebsd-src/main/sys/i386/i386/pmap.c)
  invalidates a replaced PTE only when it had `PG_A`. Decision: a VGA window
  PTE swap (`as_replace_pages`) invalidates only entries replaced with A or
  D set; an entry read with both clear is replaced by one locked exchange
  whose result decides. Every path that clears A in a present entry
  invalidates while its space is current (this function, `as_sample_page`,
  `dosvm_pool pte_sample`); a CR3 load flushes the rest (no CR4.PGE).
  Agner Fog's [P6 tables](https://www.agner.org/optimize/instruction_tables.pdf):
  `XCHG r,m` 4 µops with an implicit LOCK, CPUID 23-48 clocks; the T23 price
  of INVLPG is not published and must come from the trial.
- **Wolfenstein 3-D source** ([ID_VL.H](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/ID_VL.H),
  [ID_VL.C](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/ID_VL.C),
  [WL_DRAW.C](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/WL_DRAW.C),
  [ID_US_1.C](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/ID_US_1.C)):
  `VGAMAPMASK`, `VGAREADMAP` and `VGAWRITEMODE` are
  `cli; mov dx,..; out dx,..; sti` around each plane select of menus, text,
  latch copies and the fizzle fade (one `VGAREADMAP` and one `VGAMAPMASK`
  per pixel), so each costs a V86 CLI exit, a port exit and an STI exit;
  `ThreeDRefresh` flips with `cli; out 3D4h,0Ch; out 3D5h,hi; sti` once per
  frame; `TEDLEVEL n NOWAIT` (US_Startup) starts a map directly (used by the
  replay). Decision: a lean V86 CLI/STI exit, the instruction executed by
  `v86_emulate` itself (virtual IF, STI shadow, TF), the shadow retired at
  the next foreign boundary as on the ordinary path, every lean_before and
  lean_after obligation kept.
- **FreeVGA CRTC registers** ([Start Address High/Low, 0Ch/0Dh](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/crtcreg.htm)):
  the start address selects the displayed page; a page-flipping game draws
  into the other pages. Windows 3.x/9x refreshed a windowed DOS display on
  a timer (`WindowUpdateTime`, 50 ms default; see
  `t23-dos-window-lag-2026-10-25.md`). Decision: while a graphics-mode,
  non-VBE guest changed its start address within the last 250 ms, the
  presenter wakes only on displayed-page changes (start address, display
  registers, palette, mode), not on every register write or back-buffer
  store; each sample then shows a completed frame. After 250 ms without a
  flip, and in text and VBE modes, the store wake is unchanged.
- **Scheduler** (`task.c` rq_pop, `FOCUS_BUDGET` 8 of 10 ticks; the desktop
  group's 25 % share): a woken DOS Box presenter (base 9) waited behind the
  focused VM (base 7) for the VM's focused ticks (trial: WAIT dispatch 7.7
  ms average for Wolfenstein, 12.8 ms for DOOM, 100 ms maximum). Decision:
  from the presenter's VIDEO wake until it runs, at most 20 ms, the VM's
  focus preference is suspended (`dosvm_task_focus`), never while the VM
  is inside a guest hardware-interrupt handler (DMX's timer service sends
  its EOI early and re-enables interrupts; the edges of a longer absence
  re-enter it, and four deep it returns without EOI, f3-12); IRQ urgency
  (`dosvm_task_irq`) and the desktop share are unchanged. Corrected in round
  3: the suspension also lifted the 2-tick cap on any task picked while the
  focused VM was ready (`task.c`), so the presenter or another base-9 task
  could run a full 10-tick quantum; round 3 replaces it (below).
- **Evidence for the next trial.** The desktop's `[desktop] accel` record
  (every 2 s window with engine work, published once a minute) now carries
  `composite_us`, `present_us`, `presents` and `scene_px`, so a DOS window
  frame's composition and display requests are priced separately from the
  kernel conversion (`vm_perf_present`); `[vm-audio] cadence_clock` carries
  `cli_control`, the PM CLIs proven by the control-flow decode, which are
  never cached as sites and restart the seed demand behind DOOM's
  window-only return-tracker checkpoints (trial bank n=4: 10,674 `db`
  exits, 377 ms; full screen 0 armed). The host DOOM replay does not arm
  the tracker (`ret=0`), so its cause is not fixed here.

### Results

Replay `tests/host/dos/window_parity.py` (opt-in, not a suite): the
pinned DOOM 1.9 shareware, Wolfenstein 3-D 1.4 shareware and Warcraft II
demo on the production monitor (libx86emu guest runner), every exit priced
at trial `5bb73dba`'s measured average for its cause (`GUEST_T23_MODEL=fixed`,
deterministic), plus a DOS Box and desktop presentation model at the
trial's per-row prices (13.9 us kernel conversion, ~7 us DOS Box copy,
~50 us desktop per changed row, 25 % desktop share). Calibrated with a
starved presenter, round 1's sources reproduce the trial's DOOM window
bank n=4 (shown 8.6 against 6.8 frames/s, guest/monitor/off-CPU 195/360/447
against 208/357/435 ms/s) and Wolfenstein's shown rate (12.8 against 11.6).

| Shown frames per s (factor full/window) | Full screen | Round 1 | Round 2, presenter still starved | Round 2, presenter dispatched |
| --- | ---: | ---: | ---: | ---: |
| DOOM (attract demo) | 33.15 | 8.60 (3.85) | 9.35 (3.55) | 13.90 (2.38) |
| Wolfenstein 3-D (E1M1) | 70.06 | 12.84 (5.46) | 14.74 (4.75) | 22.55 (3.11) |

The last column assumes the dispatch change gives the presenter up to 20 %
of the CPU after a 1 ms delay (the replay parametrizes the scheduler); the
next trial's `vm_perf_presenter` waits and `[cpu]` shares confirm or refute
it. Warcraft II's demo title is static in the replay (0.22 changed
frames/s) and gives no in-game figure.

Measured on the replay: INVLPGs per DOOM remap 16 → 1.5 (334,009 →
33,443 per s, at an equal presenter share); presenter wakes per presented
frame 1.80 → 1.00 (DOOM, 27.25 → 16.10 wakes/s); Wolfenstein's V86 CLI/STI
exits lean (priced 3.02 instead of 7.85/8.87 us).

Remaining, with what-ifs on the round-2 sources: the desktop stage caps
the shown rate at 0.25 / (rows x 50 us + 250 us), 14.2 fps for DOOM's 346
changed rows and 22.7 for Wolfenstein's 213, where both settle. A 4x cheaper
presentation pipeline gives Wolfenstein 64.77 (1.08x) and DOOM 26.00
(1.27x); DOOM reaches 32.65 (1.02x) only if its ~1,660 trapped plane
selects per frame are also 4x cheaper. Decision for the lead: parity needs
structural work, not further tuning of this path: (a) a scanout region for
an unobscured DOS window on the existing presentation lease, or (b) the
VM's native frame expanded and scaled by the display engine (the model's
STRETCH table qualifies only 3dfx row replication), both with a contract and
a driver-pack hardware sequence; (c) a plane-select fast path or a
remap-free planar representation for Mode Y games. Interim: a desktop
"composed" acknowledgement so the DOS Box converts only frames the desktop
shows (Wolfenstein: 35.6 conversions/s for 22.6 shown).

Finding (pre-existing, not changed): with a single 7 ms off-CPU block per
100 ms the DOOM replay stalls at ~12.5 s; DMX's IRQ0 handler sends its EOI
early, re-enters four deep and returns without EOI (f3-12), after which
IRQ0/IRQ7 stay blocked. The trial's stretches are 1-4 ms (one 11 ms), so
the replay spreads its stretch; the presenter dispatch never yields inside
a guest IRQ handler for this reason. Whether owed IRQ0 edges should wait
for the handler's return rather than its EOI is an IRQ-delivery decision
for the lead.

Host suites green: full DOS body 61,053,171 checks, fx-27 622,987,
t23-dosperf 117,276, fx-doswin3 rounds 1 and 2, fx-16, WAIT_PRESENT, file
API, unified DOS (H57: 7 CPU boots), kernel latency, VM, audio, VGA x86,
display names/model/commands, presenter, Savage family, splash, desktop
accel/performance/native, DOS Box properties, seven i686 production units
with the kernel flags. Commands and the next-trial comparison table are in
`build/codex/fx-doswin3-report.md` (Round 2).

## Round 3 (2026-10-27): review fixes, direct DOS window presentation, composed acknowledgement

Host evidence only, on round 2's sources (`dddd59079`); no QEMU, image
build or hardware run. Review: `build/lead-scratch/fx-doswin3-review2.md`
(no P1, three P2, four P3).

### Sources and decisions

- **TLB invariant (review P3-2).** Re-checked against the text of Intel SDM
  Vol. 3A rev. 090 ([PDF](https://cdrdv2-public.intel.com/874249/253668-090-sdm-vol-3a.pdf),
  extracted locally): the sentence "the processor does not cache a
  translation for a page number unless the accessed flag is 1 in each of
  the paging-structure entries used during translation; before caching a
  translation, the processor sets any of these accessed flags that is not
  already 1" is in **5.10.2.3 "Details of TLB Use"** (5.10.2.2 is "Caching
  Translations in TLBs"), as round 2 cited. **5.10.4.3 "Optional
  Invalidation"**: "If a paging-structure entry is modified to change the
  accessed flag from 0 to 1, no invalidation is necessary (assuming that an
  invalidation was performed the last time the accessed flag was changed
  from 1 to 0). This is because no TLB entry or paging-structure cache entry
  is created with information from a paging-structure entry in which the
  accessed flag is 0." Preconditions of `as_replace_pages`' accessed-only
  INVLPG, now written next to it (`vmm.c`) and in `mm.h`: one CPU (no
  application processor is started), CR4.PGE and CR4.PCIDE clear
  (`entry.asm` clears PGE; `mtrr.c` only restores CR4, `fpu.c` changes only
  OSFXSR/OSXMMEXCPT; nothing sets PCIDE), and every writer that clears A in
  a present entry invalidates while its space is current (`as_sample_page`,
  `dosvm_pool` `pte_set`/`pte_sample`, `dosvm_fullscreen` `map_aperture`,
  `dosvm_xms`, `uaddr` `set_pte`/`alias_sample`, `as_replace_pages`
  itself). Revisit if SMP, PGE or PCID arrive. AMD: no equivalent sentence
  was found in a search of the AMD64 APM Vol. 2; the cross-vendor basis for
  the baseline's AMD desktops is FreeBSD's i386 `pmap_enter`, which applies
  the same rule on every x86 vendor.
- **Presenter slice (review P2-1).** Round 2's suspension of the VM's focus
  also removed the 2-tick cap that `task.c` puts on any task picked while
  the focused VM is ready, so the presenter, or any other base-9 task queued
  ahead of it, could run a full 10-tick quantum while the VM's IRQ0 waited
  (DMX's timer period is about 7.1 ms). Decision: the VM keeps the focus;
  `rq_pop` asks a new hook, `dosvm_task_present`, inside the focus branch
  (after a pending virtual timer/SB IRQ, which still wins, and after
  foreground input/donation boosts) and dispatches only that woken presenter
  ahead of the VM, for at most `PRESENT_SLICE` = 4 ticks; every other task
  keeps the 2-tick cap. Host test (`kernel_latency_test.c`, production
  scheduler): presenter 4 ticks, VM IRQ latency at most 4 ms, another
  base-9 task at most 2 ticks; the round-2 rule as negative control gives
  10 ticks and 10 ms.
- **Visible IRQ handler guard (review P3-1).** `present_pending` now also
  holds while the virtual PIC has an in-service bit (a real-mode handler
  before its EOI), besides an open DPMI hardware-interrupt record; the
  comment no longer claims more (a V86 handler after an early EOI is not
  visible).
- **Flip hold (review P2-2).** The fixed 250 ms hold delayed Wolfenstein's
  fizzles, menus and pause screens (they draw into the shown page without a
  flip). Decision: the hold is twice the last flip gap, at least one
  refresh, at most 50 ms (`flip_hold_ms`; Windows 9x's default
  `WindowUpdateTime`, `SYSTEM.INI [386Enh]`, see
  `t23-dos-window-lag-2026-10-25.md` 3). A first or single start-address
  change (initial set, one hardware scroll) holds for one refresh. Host
  test: 15 Hz flips hold 50 ms and a store after they stop is shown within
  two gaps; 70 Hz flips hold 30 ms; a first flip 15 ms.
- **Full screen (review P2-3).** Round 2's "full-screen paths unchanged"
  was wrong: VBE full screen models its ports, and on `main` since
  fx-doomfs2 `lean_before` no longer requires modeled ports, so the lean
  V86 CLI/STI path (and the shadow retirement before it) also runs in VGA
  full screen. Its semantics are the ordinary path's; a VGA full-screen host
  case now compares both paths and the window in the eight CLI/STI cases
  (on this branch the exits stay ordinary in VGA full screen; on the merged
  tree they are lean, which the case accepts). Full-screen banks stay the
  trial's control.
- **Direct DOS window presentation (round 3 lever).** The production
  replay (`tests/host/desktop/dos_window_replay.c`) of a changed Mode Y Fit
  frame: 241,280 composed pixels, 512,000 upload bytes, 113,280 engine-copy
  pixels in 354 requests (one-row uploads alternating with one-row copies of
  the kernel's line-doubled rows); trial-derived 50 us of desktop time per
  VM row. Decision: when every damaged box shows only an unobscured DOS
  window (`client_only`: on top, no shadow, overlay, menu, chrome or other
  window over it; hardware pointer or a software pointer that no damaged box
  touches; no Magnifier; surface 1:1 with the content), the desktop presents
  the damage straight from the window's own surface through the kernel's
  ordinary surface present (`grants.c grant_present`, as for its own
  frame): one request per damaged run of rows, nothing composed. Its RAM
  frame keeps older pixels there (`direct_stale`) until a frame that moves
  or copies windows, draws a software pointer over them, shows the
  Magnifier or damages them recomposes and presents them first; that frame
  skips the scroll and old-pixel command plans, which compare the RAM frame
  with the scanout. Print Screen and the graphics benchmark compose them
  first (`desk_direct_settle`). No display-model or driver change, no family
  name. Price on the T23: 965,120 bytes a frame at its 134 MB/s upload rate
  (f4-20; 1024x768x32 with a write-combining MTRR in trial `5bb73dba`) =
  7.2 ms, 18.0 us per VM row.
- **Composed acknowledgement.** After a frame's DAMAGE the DOS Box sends
  the scene-less `DESK_GFX_FRAME` (f2-59's completion protocol, the 64-byte
  form, now accepted from a v2.0 client and from a launched DOS window
  only); the desktop answers `DESK_GFX_PRESENTED` once the frame is on screen
  (`-EAGAIN` at once when nothing is pending or the window is minimized). A
  frame carries the marker only when none is outstanding; the DOS Box may
  convert one more frame, then waits for the answer (at most 100 ms, serving
  its channel with `poll`), so it is at most one frame ahead of the desktop
  and converts no frame the desktop would coalesce away. It waits only once
  the desktop has answered once. A serial form (no overlap of the two
  stages) was measured and rejected (results). No public header changed
  (`protocol.h` untouched; `protocol.c` parse rules only).

### Results

T23 replay (`tests/host/dos/window_parity.py`, round 2's fixed trial
prices; desktop direct price 18.0 us per VM row, the 965,120-byte upload of
a 400-row Mode Y frame at 134 MB/s; 60 Hz desktop cadence; DOS Box share
0.20 as in round 2):

| Frames shown per s (factor full/window) | DOOM | Wolfenstein 3-D |
| --- | ---: | ---: |
| full screen (round-3 sources) | 33.25 | 70.03 |
| round 2 | 13.90 (2.39) | 22.55 (3.11) |
| round 3, review fixes only | 13.95 (2.38) | 22.55 (3.11) |
| round 3 delivered (direct presentation, one-frame-ahead acknowledgement) | 19.65 (1.69) | 38.61 (1.81) |
| window obscured (composed fallback) | 13.65 | 22.16 |

Full screen is unchanged within 0.3 % (Warcraft II 43.92 against 43.93 M
instructions per s). The replay's Warcraft II demo title is static; by the
trial's row prices its in-game frame now costs 50 instead of 81 us per row.
The acknowledgement never holds the DOS Box when the desktop presents
directly; with the composed fallback it removes Wolfenstein's 13 dropped
conversions per s (409 -> 349 ms of presentation CPU per s). Remaining per
DOOM frame: 8.8 ms of plane-select exits and 13.7 ms of presentation CPU;
Wolfenstein shows 38.6 of 57.5 rendered frames, limited by the DOS Box
stage (20.9 us of kernel conversion and viewport copy per VM row), then the
desktop's 60 Hz cadence. What-ifs: with no DOS Box viewport copy and
engine-doubled rows, DOOM 24.00 (1.39) and Wolfenstein 56.71 (1.23); plus
4x cheaper plane selects, DOOM 30.65 (1.08).

Host tests: full DOS body (61,112,054 checks), fx-doswin3 rounds 1-3,
fx-27, t23-dosperf, fx-16, WAIT_PRESENT, the production game launch
(`/Programs/dosbox` over the production client), file API, unified DOS,
kernel latency (incl. the presenter-turn case), VM, audio, VGA x86, display
names/model/commands, presenter, Savage family, splash, desktop accel and
performance (incl. the direct DOS window replay), the DOS Box loading
replay and properties, eight i686 kernel units with the kernel flags; of the
34 desktop model tests 33 pass and `test_shell_launcher` fails, identically
on the committed round-2 tree (pre-existing). Commands and the next-trial records are in
`build/codex/fx-doswin3-report.md` (Round 3).

### Round 3 review fixes (2026-10-27)

Review `build/lead-scratch/fx-doswin3-review-0314.md` (Codex, probes on
`2f24fcecc`): three P2 and one P3, all fixed with regressions that fail on
the reviewed commit. The direct path's older RAM pixels (`direct_stale`)
are now also excluded from the desktop's equality suppression of repeated
client frames (A -> B shown directly, then back to A, was dropped) and from
scroll plans (a translated source inside them copied different scanout
pixels); `desk_direct_settle` (Print Screen, graphics benchmark) runs one
forced composed frame, so a pending window move is resolved before its RAM
copy source is recomposed, instead of composing the boxes with the new
geometry. The presenter-first dispatch also waits for any requested,
unmasked virtual IRQ (`dosvm_pic_pending`), not only the timer and SB IRQs
that `dosvm_task_irq` makes urgent (a keyboard or UART request). Details,
negative controls and commands: `build/codex/fx-doswin3-report.md`,
"Round 3 review fixes".
