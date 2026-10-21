# f5-12B: MCI, ACM, Video for Windows playback (host evidence and decisions)

Date: 2026-10-20. Directive: `docs/directives/f5-12b-mci-codecs-avi-cdaudio.md`.
Scope of this record: the MCI layer of `winmm` (`waveaudio`, `sequencer`),
the `avivideo` device (`mciavi32.dll`), the msacm32 discovery entry points and
PCM converter, avifil32 sample/time conversion, msvfw32 DrawDib and the
MCIWnd subset. `cdaudio` is not implemented: its transport needs the
reviewed F4 optical amendment (proposal in `build/codex/f5-12b-amendment.md`).
Nothing here was run on QEMU or hardware. The subsequent F5-12B2 CD backend
and its approved contract are recorded in [f5-12b2-cd-audio.md](f5-12b2-cd-audio.md).

## F5-12B continuation research (2026-10-21)

The current base contains both accepted deliveries. The continuation preserves
the lead's digital-primary/analog-fallback decision. Before changing code,
the following primary references were checked against production sources:

- [AVIStreamRead](https://learn.microsoft.com/en-us/windows/win32/api/vfw/nf-vfw-avistreamread)
  and [AVISTREAMHEADER](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/api/avifmt/ns-avifmt-avistreamheader):
  a nonzero dwSampleSize groups multiple fixed-size samples in a chunk. The
  previous reader returned chunk counts for PCM; correct Length/Info/Read
  to use sample units, support reads crossing chunk boundaries, and adapt
  MCI AVI audio to request bounded batches of whole PCM frames/ADPCM blocks.
- [AVI RIFF file reference](https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference)
  and [AVIOLDINDEX](https://learn.microsoft.com/en-us/previous-versions/ms779634(v=vs.85)):
  parse optional idx1 entries with bounds, validate their chunk identities,
  sizes and offsets against the already scanned movi data, and accept both
  documented movi-relative and historical absolute offsets. Keep physical
  stream order; do not trust an index to authorize reads outside movi.
- [Microsoft Multimedia Standards Update, revision 3](https://mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/RIFFNEW.pdf):
  PCMWAVEFORMAT is 16 bytes. acmStreamOpen already handles that boundary;
  acmFormatSuggest must also avoid copying a nonexistent cbSize field.
- [FFmpeg Cinepak implementation](https://github.com/FFmpeg/FFmpeg/blob/n7.1/libavcodec/cinepak.c)
  and [MS Video 1 implementation](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/msvideo1.c)
  are upstream format-behavior references, alongside the existing format
  notes below. No new upstream decoder code is copied. The f5-14C decoder
  sources/provenance remain in place.

Exact checks, host cost and sandbox restrictions for this continuation are
recorded separately in `build/codex/f5-12b-report.md`.

Final continuation evidence: `nice -n 19 make CAP= apps` passes. The focused
media/codec run has 19 passes, one seccomp PE-adapter skip and five failures
solely at LeakSanitizer shutdown (ptrace is unavailable). Codec reproducibility,
24000 codec fuzz iterations and the exactly sized PCM16 regression pass;
the media harnesses retain leak detection. Separate saved-artifact checks
match all three pinned traces and nine independent AVI/GDI pixel oracles.
Final Win32 discovery runs 258 tests: 213 pass, 19 skip, 25 fail at the same
LeakSanitizer shutdown, and the unchanged dialog harness fails to link
`host_pixel_format`. Source/fixture hashes remain stable across this run and
the final apps build. No green full-suite, guest or hardware result is claimed.

## Sources checked before implementation

- MCI notification: [MM_MCINOTIFY](https://learn.microsoft.com/en-us/windows/win32/multimedia/mm-mcinotify)
  (aborted/superseded/successful/failure; a new play keeps the previous
  play's notification unless it changes the end position or direction;
  pause does not abort; a failing command sets no notification),
  [The Notify Flag](https://learn.microsoft.com/en-us/windows/win32/multimedia/the-notify-flag).
- Wait and break: [The Wait Flag](https://learn.microsoft.com/en-us/windows/win32/multimedia/the-wait-flag)
  (CTRL+BREAK default; a cancelled wait returns without interrupting the
  command), [MCI_BREAK](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-break),
  [mciSetYieldProc](https://learn.microsoft.com/en-us/previous-versions/dd757163(v=vs.85)),
  [mciGetYieldProc](https://learn.microsoft.com/en-us/previous-versions/dd757159(v=vs.85)).
- [mciGetCreatorTask](https://learn.microsoft.com/en-us/previous-versions/dd757155(v=vs.85)),
  [mciGetDeviceID](https://learn.microsoft.com/en-us/previous-versions/dd757156(v=vs.85)),
  [mciGetErrorString](https://learn.microsoft.com/en-us/previous-versions/dd757158(v=vs.85)),
  [MCIERR return values](https://learn.microsoft.com/en-us/windows/win32/multimedia/mcierr-return-values).
- Command strings: [open](https://learn.microsoft.com/en-us/windows/win32/multimedia/open),
  [status](https://learn.microsoft.com/en-us/windows/win32/multimedia/status),
  [set](https://learn.microsoft.com/en-us/windows/win32/multimedia/set),
  [play](https://learn.microsoft.com/en-us/windows/win32/multimedia/play),
  [put](https://learn.microsoft.com/en-us/windows/win32/multimedia/put),
  [where](https://learn.microsoft.com/en-us/windows/win32/multimedia/where),
  [window](https://learn.microsoft.com/en-us/windows/win32/multimedia/window),
  [Command Shortcuts and Variations](https://learn.microsoft.com/en-us/windows/win32/multimedia/command-shortcuts-and-variations),
  [MCI_MAKE_TMSF](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-make-tmsf),
  [MCI_MAKE_MSF](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-make-msf),
  [MCI_DGV_RECT_PARMS](https://learn.microsoft.com/en-us/windows/win32/api/digitalv/ns-digitalv-mci_dgv_rect_parms).
- Codecs: [IMA ADPCM recommended practices rev. 3.00](https://www.cs.columbia.edu/~hgs/audio/dvi/IMA_ADPCM.pdf),
  [Multimedia Standards Update rev. 3.0](https://mmsp.ece.mcgill.ca/Documents/AudioFormats/WAVE/Docs/RIFFNEW.pdf)
  (ADPCMWAVEFORMAT, DVI/IMA ADPCM, PCM data layout),
  [ADPCMWAVEFORMAT](https://learn.microsoft.com/en-us/previous-versions/ee415686(v=vs.85)),
  [Cinepak](https://wiki.multimedia.cx/index.php/Cinepak),
  [Microsoft Video 1](https://wiki.multimedia.cx/index.php/Microsoft_Video_1),
  [Microsoft RLE](https://wiki.multimedia.cx/index.php/Microsoft_RLE),
  [Bitmap Compression (BI_RLE8/BI_RLE4)](https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-compression).
  The decoders are the f5-14C implementations, unchanged.
- Standard MIDI Files 1.0, part of the MIDI 1.0 Detailed Specification
  ([MIDI Association: MIDI files](https://midi.org/about-midi-part-4midi-files),
  [specification download](https://midi.org/the-midi-association-offers-free-download-of-the-midi-1-0-specifications-document)):
  MThd/MTrk, variable-length quantities, running status, meta tempo FF 51,
  SMPTE division; the RIFF RMID wrapper (Multimedia Standards Update above).
- System configuration: Windows 98 keeps 16-bit `[mci]` drivers in
  System.ini (Resource Kit); the 32-bit names mciwave/mciseq/mcicda/mciavi32
  are the NT `MCI32` key. CiukiOS uses a private table (below).

## Repository comparison

f5-12A left placeholder MCI entry points in `sdk/win32/audio/misc.c`
(returning a fixed error); they are removed. The frozen f5-12A boundary
(`audio.h`, `CiukiAudio*`, waveOut, midiStream, the mixer) is used unchanged.
The f5-14C decoders are used unchanged; acm.c gains a PCM-converter branch
and reads only 16 bytes of a PCM source format (a 16-byte PCMWAVEFORMAT has
no cbSize); avi.c gains AVIStreamSampleToTime/TimeToSample.

## Decisions

1. **MCI core in winmm, devices behind a private boundary** (`mci.h`).
   `waveaudio` and `sequencer` are compiled into winmm; `avivideo` is
   `mciavi32.dll`, loaded on first open through its `CiukiMciDriver` export
   (winmm keeps no static USER/GDI/codec imports). `cdaudio` is a known type
   that reports `MCIERR_DEVICE_NOT_INSTALLED` (306) until the CD transport
   lands. Extensions: wav, mid/rmi/midi, avi.
2. **Notifications** follow the MM_MCINOTIFY rules: drivers post ABORTED
   when they cancel a pending operation (stop, seek, close, a play with a new
   end); a later `notify` request supersedes; a continued play keeps its
   notification; pause/resume keep it; async completion posts SUCCESSFUL or
   FAILURE (`PostMessage(hwnd, MM_MCINOTIFY, flag, id)`).
3. **Wait**: the caller polls the device with the yield procedure or the
   break key (default VK_CANCEL, `GetAsyncKeyState` bound at run time); a
   break returns 0 and leaves the command running (SDK "The Wait Flag").
4. **Auto-open** closes the device when its command completes, fails or is
   aborted (SDK shortcuts page; Wine keeps it open). `notify` on an
   auto-opened device is `MCIERR_NOTIFY_ON_AUTO_OPEN`. `open` returns the
   device id as its string result.
5. **Return strings**: integers in decimal, MSF/HMS as `mm:ss:ff`, TMSF/SMPTE
   as `tt:mm:ss:ff` (least-significant byte first), rectangles as
   `x y w h`, resources ("playing", "true", "milliseconds", "PPQN", ...).
   The command interface reduces resource returns to their low word
   (`MCI_MODE_PLAY`, TRUE/FALSE).
6. **Default time formats**: waveaudio milliseconds; sequencer milliseconds
   for PPQN files and the file's SMPTE rate for SMPTE files (SDK: "the
   sequence file sets the default format"); avivideo frames.
7. **waveaudio** streams 4 x 0.25 s headers from the MCI worker; ADPCM is
   decoded through msacm32 (bound by name); positions are frames, and
   milliseconds round up so that a seek reads back the same value.
8. **sequencer** merges tracks on the fly (file <= 1 MiB, <= 128 tracks,
   <= 2048 tempo changes, malformed events rejected), converts every event to
   absolute microseconds through the tempo map and plays them on midiStream
   with division 1000 and tempo 1000 (1 tick = 1 us). Pause stops the stream
   and resume re-chases controllers/programs/bend/pressure from the paused
   position (midiStreamPause is not used). `set tempo`, `master`, `offset`
   and `port none` playback are not provided (clean MCI errors).
9. **avivideo** draws through DrawDib into the default window (created
   hidden at open, shown at play) or a caller window (`window handle`);
   frames follow GetTickCount from the play start with the sound track
   started at the same instant through ACM and waveOut; completion waits up
   to 250 ms for the sound tail. `play fullscreen` is approximated by a
   screen-sized top-level window with the movie scaled by an integer factor
   (no display-mode switch); the window's own thread restores it. Reverse
   play, speed other than 1000, record/save/freeze are unsupported errors.
10. **msvfw32 binds gdi32/user32/winmm at first use** (GetProcAddress on the
    export names listed in `scripts/win32/codecs.py` BOUND, checked against
    the store by the media test): the codec images keep importing kernel32
    only, so decoding never loads USER/GDI (f5-14C's H1 probe stays valid).
    MCIWnd draws no toolbar, trackbar, menu or error dialog.
11. **ACM**: drivers 1 (PCM converter), 2 (MS ADPCM), 17 (IMA ADPCM) with
    standard formats at 8/11.025/22.05/44.1 kHz; the PCM converter changes
    sample size, channels (stereo->mono averages) and rate by whole-frame
    repetition/dropping with a persistent per-stream phase, in 32-bit
    arithmetic (the codec images link no compiler runtime).

## Host evidence (this worktree)

`nice -n 19 python3 -B -m unittest tests.host.win32.media.test_media -v`:

- `mci_test.c` + `mci_host.c`: production winmm MCI/waveOut/midiStream/
  mixer and msacm32, deterministic clock; trace pinned in
  `tests/host/win32/media/traces/mci.txt` (errors, waveaudio, sequencer,
  auto-open, sysinfo, command interface, W entry points). ADPCM playback is
  compared sample for sample with the f5-14C scalar reference decode in the
  mixed output (full file and a mid-file range); MIDI message frames are
  compared with an independent tempo-map oracle.
- `avi_test.c`: production mciavi32, avifil32, ICM decoders, msacm32,
  DrawDib, MCIWnd and the gdi32 DIB engine over modelled windows; trace in
  `traces/avi.txt`; nine dumped frames (default window, caller window 2x,
  seek/step, IMA sound track, DrawDib compressed draw and DDF_UPDATE,
  MCIWnd) equal independently computed pixels; PCM and IMA sound tracks
  equal the reference decode.
- Fuzz under ASan/UBSan with LeakSanitizer enabled: 20,000 MCI iterations (mutated MIDI/WAV files and
  random command strings) and 3,000 mutated AVI files through
  open/play/step/seek/put/close; allocation balance checked.
- ACM discovery and PCM converter (split conversions against a Python
  oracle), AVIStreamSampleToTime/TimeToSample, PE store exports/imports,
  census coverage and an i686 no-SSE/MMX disassembly check.
