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
