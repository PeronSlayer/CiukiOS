# F5-12A research and user-mode audio boundary

Research date: 2026-10-19. Host evidence only; integration report is
`build/codex/f5-12a-report.md`.

## Primary references and implementation decisions

- Microsoft DirectX SDK: [buffer cursor semantics](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418062(v=vs.85)),
  [wrapped locking](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418073(v=vs.85)),
  [buffer descriptors](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416820(v=vs.85)).
  Keep separate rendering and consumed cursors, return two lock spans, and
  expose a software device. Primary locking requires WRITEPRIMARY.
- Platform SDK: [waveOutOpen callback types](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutopen),
  [waveOutReset ownership](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveoutreset),
  [MIDIEVENT padding and deltas](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/ns-mmeapi-midievent),
  [midiStreamOut preparation](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midistreamout),
  [multimedia timers](https://learn.microsoft.com/en-us/previous-versions/dd757634(v=vs.85)).
  Completion follows consumption, reset returns queued ownership, MIDI
  deltas retain fractional time, missed timer ticks coalesce without bursts.
- [TinySoundFont upstream](https://github.com/schellingb/TinySoundFont),
  `tsf.h` v0.9, upstream Git blob `a81f25d5ca2e210720d646dec2dbfaeb119acb09`,
  MIT (notice in the header). The original eager float sample allocation
  would double the 148,398,306-byte bank's sample storage and be private.
  A narrow local adapter borrows read-only S16 data instead. Private
  preset/voice/channel allocations use the bounded personality allocator.
  Offset macros use `offsetof` rather than subtracting null pointers;
  voices steal release-first then oldest active. Unchanged pitch and gain
  are cached to remove repeated transcendental calls. These are local changes.
- [Creative/E-mu SoundFont specification](https://www.synthfont.com/sfspec24.pdf):
  RIFF tables, sample rates, sample/loop addresses and generator units
  constrain the reduced-bank conversion; the script refuses unsupported
  nonzero coarse address offsets rather than modifying their meaning.
- [FluidR3 source licence and authors](https://github.com/Jacalz/fluid-soundfont/tree/master/original-files)
  and [Buildroot's source archive pin](https://lists.buildroot.org/pipermail/buildroot/2019-December/258213.html).
  Bank and synthesizer licences are separate. The full bank's original
  bytes are required; authored test waves are not a substitute.
- [GeneralUser author documentation](https://github.com/mrbumpy409/GeneralUser-GS/blob/main/documentation/README.md)
  and [author's site](https://schristiancollins.com/generaluser.php) are the
  Q6 alternative. No GeneralUser or Microsoft Roland bank was imported.

Compared with `src/kernel/audio/stream.c`, `mixer.c` and the f4-31 audit:
`accepted_frames`/`consumed_frames` count process-stream frames; backend
output/fetched counters are different clocks. Never use wall time or the
hardware fetch pointer as a DirectSound play cursor. Native stream rate is
kept stable after opening; source-rate changes are software resampling.
No kernel audio code is changed.

## Frozen f5-12B boundary

`sdk/win32/audio/audio.h`, version 1, is private to the personality, not a
new kernel ABI. `CiukiAudioAcquire/Lock/Unlock` live in winmm and are
imported by dsound. The process owns one stream and worker. Source registry
quota: 64; queued wave headers: 64 globally; notification entries: 64
globally; callback records: 512; timers: 16. Allocation ceiling: 4 MiB,
including the static engine and a 128 KiB worker-stack/bookkeeping reserve,
excluding shared bank pages and application-owned wave data. `wa_alloc/wa_free` account personality allocations.

`wa_render` writes S16 stereo, integer rational linear interpolation with
truncation toward zero, Q15 per-buffer dB/pan gain (pinned table), integer
stream gain, then saturation. State persists across batch boundaries.
`wa_pump` retains partial writes and never accepts more than 512 frames
beyond the consumed counter. A 512-frame ledger stores each source's
position, rational phase and epoch for each output frame. Cursor reads
retire that ledger using AUDIO_GET_STATUS. Stop/seek/reset invalidate old
cursor epochs. The write cursor leads the consumed position by 512 output
frames converted to source frames, rounded upward and wrapped.

Callbacks carry an output-frame deadline. `wa_dispatch` retires ownership
and dispatches at most 64 records per pass, outside the personality lock
while calling application code. Notifications use crossing-time order,
then declaration order for equal offsets, with stop after crossings.
Insufficient callback capacity stops rendering until consumption frees it.
Timer polling is 1 ms best effort; scheduler stalls and application
callbacks add jitter. This is not a hardware real-time guarantee.

The shared-bank selection proposal uses reduced at 128 MiB and full only
at installed >=384 MiB and available >=224 MiB. These conservative numbers
are not a substitute for measuring the integrated kernel/desktop baseline.
The zero-copy sample adapter has no decoded cache (0 bytes). File sections
share resident pages while a named mapping exists; retaining them once per
boot requires a service owner outside this directive's allowed code.


Host verification: the final Win32 suite passed 89 tests (10 environment
skips), including seven audio checks; the unchanged kernel audio suite
passed 11,217,090 production checks plus VM/target audits. The authored
64-voice synth uses approximately 35.5 ms thread CPU per 0.99846 s audio
on this host. A stated 4.2x clock-only extrapolation is about 14.9%, without
IPC/cache/x87 calibration. It does not qualify FluidR3's <=15% gate.
Host engine plus peak authored synth metadata is 883,376 bytes, excluding
source buffers/shared bank pages; full-bank and integrated budgets remain
unmeasured. Detailed measurements and limitations are in the report.


File MMIO uses a bounded 8 KiB read buffer and supports seek/RIFF descent
without staging whole files; the authored host fixture exercises reads
beyond 2 MiB. The worker retains a winmm loader reference outside the audio
lock so application FreeLibrary calls cannot unmap its executing code.
Timer-only use starts that worker without opening an audio descriptor.

Final API reference generation and `nice -n 19 make CAP= apps` passed after
the final changes. Missing-bank admission fails before image mutation.
DirectSound export ordinals use the existing pinned spec as inventory only;
duplicate buffers allocate metadata without temporarily copying PCM.
