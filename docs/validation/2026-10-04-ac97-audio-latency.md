# AC'97 queue latency change, 2026-10-04

## Source contract and change

The Intel ICH4 AC'97 programming interface defines the bus-master descriptor
length in 16-bit samples and exposes the current/last-valid descriptor indices
(CIV/LVI). The driver writes one descriptor per allocated page, so the sample
count is `CVDEV_AUDIO_FRAMES * 2` for stereo. The [ICH4 datasheet, AC'97
register section](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82801db-io-controller-hub-4-datasheet.pdf)
is the hardware reference.

The monitored-session output used to queue eight 1024-frame descriptors at
44.1 kHz: 23.22 ms per descriptor and about 185.8 ms of software lookahead.
The queue count remains eight, but each descriptor is now 256 frames: 5.81 ms
each, or about 46.4 ms queued at 44.1 kHz (42.7 ms at 48 kHz). Each stereo
descriptor occupies 1024 bytes, within its existing 4 KiB allocation. This
reduces the driver's queued audio lead without changing the guest SB16/OPL
models or allocated page count.

## Evidence and limits

The pre-change capture is
`build/tests/desktop-web-audio-2026-10-04/audio-full-report/audio.wav` and its
20 one-second device reports are in the adjacent `results.json`. During the
captured Doom interval, all 20 reports had `underruns=0`, `ac97_status=0`,
`rate=44100`, and an AC'97 queue of 7 or 8. `buffers_rendered` rose from 1332
to 2151 (about 43 descriptors/second), consistent with 1024-frame descriptors
at the game output rate. The WAV's selected 21.55-second interval had median
10 ms window RMS 358 and 108 near-zero windows, grouped into intermittent
60–100 ms spans. This establishes that the guest queue did not report
underruns; it does not show that the fixed software lookahead is the only
reason the owner hears lag, or explain every quiet WAV interval.

The full-image runner selects the first available backend in the order
PipeWire, PulseAudio, ALSA, SDL (unless `QEMU_AUDIO_BACKEND` is set), and
attaches AC'97. It does not currently set a backend-specific latency. QEMU's
[audio-device documentation](https://qemu.readthedocs.io/en/master/system/qemu-manpage.html)
describes requested PulseAudio/PipeWire latency and backend buffering options;
those are separate from the guest's AC'97 descriptor queue. The existing WAV
capture measures emitted PCM, not physical-output latency through the selected
Linux host backend. This change therefore targets the measured 185.8 ms guest
lookahead and leaves host-backend latency as a separate measurement.

The post-change full Linux VM run is recorded in
`build/tests/desktop-web-audio-2026-10-04/web-audio/results.json` with its WAV
and screenshot alongside it. Doom reached a new game, accepted input and
exited normally. Across twenty one-second reports, `buffers_rendered` grew
at approximately 172 descriptors/second, consistent with 256-frame buffers.
The underrun counter stayed at one throughout the observed interval. The
diagnostic acquisition immediately before that interval stopped the VM to
locate the report in physical memory; the first sample showed a depleted
queue. It cannot establish whether that initial underrun came from normal
gameplay or from the diagnostic pause. No new underrun was recorded after
sampling began. Subsequent queue samples generally contained 6–8 descriptors.

This validates the shorter guest queue and continued playback, not physical
speaker latency or the absence of audible artifacts through PipeWire/PulseAudio.
The owner still needs the final Linux audio backend path to be assessed by
listening; the automated run captured PCM through QEMU's WAV backend.
