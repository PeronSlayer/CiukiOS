# FX-VMClock: per-VM audio turn budget

Directive: `docs/directives/fx-vmclock.md`. Host-only evidence; no physical
latency or image qualification is claimed.

## Research and decision

- Creative's [Sound Blaster Series Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf),
  chapter 3, describes DMA block completion, auto-init continuation and exit
  at the current block boundary. Keep all sample, DMA, boundary and IRQ work.
- id Software's [Quake DOS driver, `BLASTER_GetDMAPos`](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/snd_dos.c)
  polls DMA position and acknowledges DSP completion. Keep DMA progress
  independent of guest interrupt entry and acknowledgement.
- Windows 98 Resource Kit, owner media, `build/rskt/wrkc12_1.htm`,
  “WDM Audio”, describes kernel-mode legacy Sound Blaster/MPU emulation
  and mixing. The existing dedicated VM clock and mixer ownership stay intact.
- Clang's [SanitizerCoverage](https://clang.llvm.org/docs/SanitizerCoverage.html)
  documents compiler-inserted edge/load/store/division callbacks;
  [UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)
  documents alignment/null checks. The replay charges executed generated
  blocks, including successful sanitizer checks; source layout can change
  its cost without adding a guest-visible operation.

The directive names no external sources itself. The references above are the
existing audio contract's primary sources and the instrumentation documentation.

At base `41732d190`, the unchanged replay fails only `vm-clock`: per-VM audio
252 us / 250 us, 84,389 samples; the worst case includes the tiny auto-init
40 ms catch-up (1,800 blocks in 80 turns). Temporarily instrumenting nested
functions isolates the increase to `sb_advance` itself. Restoring only the
pre-`6dd6049fd` `dosvm.c` leaves 252 us. Restoring the earlier VM source and
changed existing fixture fragments gives 250 us. In that earlier setup,
substituting only the current `dosvm_devices.h` restores 252 us: the expanded
`dosvm_mouse` from `wt/fx-dosmouse` shifts the embedded audio state.

With Clang 23.1.3, disassembly of these two instrumented `sb_advance` functions
has 53 versus 59 static coverage guard calls and 11 versus 14 UBSan type
mismatch call sites. Repeated accesses to `v->audio.advance_end` gain successful
check/edge work. DMA, transfer, range, boundary and wake catch-up costs remain
identical. This identifies a layout-sensitive replay regression, not new mouse
callback execution on the audio path.

Decision: snapshot the turn's immutable `advance_end` alongside its existing
`now` snapshot before the DSP loop. Only `dosvm_audio_advance` writes that
ceiling, before calling `sb_advance`; DMA consumption and block transitions do
not alter it. This removes repeated loads/checks while preserving the turn
allowance, backlog ceiling, safe points, partial DMA progress, block boundaries,
diagnostics and interrupt semantics. Keep the 250 us budget and sanitizers.

## Verification

`nice -n 19 python3 -B tests/host/irq_work_replay.py` passes after the change:

| Target | Samples | Before max us | After max us | Budget us |
| --- | ---: | ---: | ---: | ---: |
| DOS clock top half | 47,402 | 3 | 3 | 100 |
| DOS clock turn | 222,924 | 260 | 253 | 1,000 |
| DOS audio per VM | 84,389 | 252 | 245 | 250 |

All 28 handler targets pass. The audio catch-up retains 80 turns, with its
modeled total falling from 19,622 to 19,087 us; the audio maximum has 5 us
margin. These are conservative instrumented costs, not measured hardware times.
The existing tiny auto-init regression checks 45 blocks/ms, 1,800 catch-up
blocks, 79 safe points, unchanged DMA units and IRQ merging/acknowledgement.

`nice -n 19 python3 -B -m unittest discover -s tests/host -p
test_kernel_latency.py` passes all 15 tests (93.299 s).

The canonical DOS fixture recipe reports 61,036,015 checks PASS, including the
mouse and full-screen sound regressions. `nice -n 19 bash tests/host/audio/run.sh`
reports 58,756,310 production audio checks PASS. Both then exit 1 because
LeakSanitizer cannot obtain ptrace access in the implementer's sandbox. The
remaining audio recipe commands were run sequentially with failure collection,
retaining leak detection: their assertions/cost/code-generation checks pass;
four additional sanitized processes fail at the same leak-check shutdown.
All six DOS disk-trace scenarios reach their result records, then encounter the
same shutdown limitation. DOS/audio suite exit status and leak freedom therefore
remain unqualified pending a lead rerun where LeakSanitizer can complete.

The implementer report, `build/codex/fx-vmclock-report.md`, records exact commands
and reconstruction of the temporary recipe drivers. No public ABI/SDK header or
vendored code changes.
