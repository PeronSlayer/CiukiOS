# F4-07 native Music Player

## F4-30 removable-volume startup and shared Places (2026-10-18)

Research before implementation: [minimp3 usage and duration detection](https://github.com/lieff/minimp3#usage)
requires preserving unconsumed bytes and recommends 16 KiB lookahead. Its
[upstream streaming adapter](https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h)
also permits starting without a whole-stream duration scan. The current
adapter instead decodes the entire MP3 during open, then reads it again for
playback, and refills after each frame. At removable-volume latency that
delays first PCM in proportion to song length. Keep the existing worker,
publish Loading before blocking I/O, decode only the first frame at open,
and refill the compressed window in batches. Read Xing/Info frame counts
when present; otherwise show an unknown duration until natural EOF, keeping
the seek API able to replay decoded audio precisely. Do not estimate VBR
duration from the first frame's bitrate.
If a forward seek discovers the end of an unknown-length stream, clamp to
the newly established duration instead of reporting a file error.

[ID3v2.4 section 3.1](https://id3.org/id3v2.4.0-structure) defines a ten-byte
header, a synchsafe payload length and an optional footer. Skip a validated
leading tag by seeking, so large cover art does not enter the MP3 sync loop.
[newlib fread](https://sourceware.org/newlib/libc.html#fread) distinguishes
short counts from stream errors: accumulate compressed input and report
actual I/O errors rather than dropping a partial frame.
[stb_vorbis v1.22](https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c)
already provides file streaming and bounded end-page duration lookup; retain
it. [Microsoft RIFF](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)
continues to govern the existing WAV chunk bounds and padding.

The shared file dialog uses the existing `MEDIA_GET_STATE` / `MEDIA_GET_VOLUME`
ABI (`src/kernel/include/ciuki/abi.h`) for mounted volume types, generations
and cached labels. Its Places column includes the four personal folders,
the fixed system root, and mounted USB/optical roots. Poll these metadata
snapshots while the dialog is open; closing the dialog releases the observer.
No new kernel ABI or desktop IPC operation is needed. Diagnostics use the
same native raw debug sink as desktop watchdog records, independently of
stdio/VFS file locks; host builds write the same records to stderr.

Written directive: `docs/directives/f4-07-music-player.md`. Research checked
2026-10-13 before implementation:

- [minimp3 upstream](https://github.com/lieff/minimp3): CC0; frame decoding
  consumes `frame_bytes`, returns frames per channel, and requires substantial
  compressed lookahead (16 KiB). Use the scalar decoder with bounded file I/O,
  not a whole-song allocation or an unbounded seek index. F4-30 replaces the
  original duration scan during open; MP3 seek replays from the start so the
  bit reservoir is populated correctly.
- [stb_vorbis upstream](https://github.com/nothings/stb/blob/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c):
  interleaved signed-16 streaming and PCM-frame seeking. Reuse the exact
  already-vendored v1.22 source; select the MIT alternative in `third_party/stb/LICENSE`.
  Its upstream limitations remain: no floor-0 or chained Vorbis streams.
- [Microsoft RIFF documentation](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-):
  chunks may appear in any order and odd payloads have WORD padding. The WAV
  wrapper bounds every chunk by the RIFF extent and supports unsigned 8-bit
  and signed little-endian 16-bit PCM, one or two channels.

Repository comparison: `apps/desktop/sound_runtime.c` opens `/dev/audio`
nonblocking; `src/kernel/audio/stream.c` accepts only 8000..48000 Hz, and the
current backend's output rate is 48000. Native Music Player queries caps and
resamples every source rate to that output rate before submitting S16 stereo.
Integer rational interpolation keeps phase across decode blocks. The stream
has no pause opcode: pause discards its queued audio by closing the handle and
replays retained PCM from the audible-frame position. Stop and seek also
close the handle, preventing stale samples from surviving a transition.
Master volume uses read-only `MIXER_GET_MASTER` and delegated desktop
`DESK_MIXER_SET`, as Volume Control already does; it never opens a privileged
mixer control handle.

A command/pump worker owns playlist filesystem operations and audio I/O.
A normal-priority reader exclusively owns decoding while active; commands join
it before seeking or replacing a decoder.
The UI sends bounded commands and reads snapshots under a short mutex; no
file parsing, decoding, directory traversal, or audio drain runs on the UI
thread. PCM pending storage is a bounded ring. Shutdown/close sets a cancellation
flag, and the UI waits asynchronously for the worker to close its resources.
Playlist limits are explicit (256 local paths, 512 bytes per path, bounded
folder recursion); `.m3u` comments and relative paths are supported, network
URLs are rejected. The image creates empty `/Documents/Music` and
`/Documents/Pictures`; Pictures appears on the desktop only when that directory
exists. Image Viewer already implements Previous/Next, Best Fit, Actual Size
and their shortcuts; retain those controls.

Implementation limits: Vorbis uses a fixed 1 MiB codec arena (oversized setup
headers fail rather than growing the heap); minimp3 keeps a 16 KiB compressed
lookahead and a roughly 17 KiB decode frame on the SDK default 1 MiB decoder-thread
stack. The PCM ring is 131072 stereo frames (512 KiB), including retained
submitted history for exact pause replay. Playlist text imports
are capped at 140000 bytes and folder recursion at eight levels; FAT32 has
no symlinks. Folder imports sort local audio paths, while M3U imports retain
entry order. Pause drops the native stream queue and retains its PCM for replay; already mixed DMA audio
may still play for the bounded transport tail, as with system sounds.

Files double-click launches a new player for audio/M3U paths. Its folder
context action, “Play in Music Player”, builds a playlist from the folder.
Within Music Player, File > Add Files and Add Folder append to the current
playlist. The existing launcher creates independent windows, so opening
another file from Files does not implicitly replace another player's queue.

Integration review found that `client.c` restricts `DESK_MIXER_SET` using
launch-owned `desk_client.mixer`, assigned only to `/Programs/mixer` in
`desktop.c`. Extend that same flag to the desktop-launched `/Programs/music`;
ordinary clients still receive EPERM and requests during shutdown receive
EBUSY. This is necessary for the directive's master-volume control and keeps
the kernel control descriptor inside the desktop.

## F4-36 contention research and implementation decision (2026-10-19)

Before implementation, rechecked the pinned [minimp3 README](https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/README.md)
and [decoder source](https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h),
[stb_vorbis v1.22](https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c),
and upstream newlib [setvbuf](https://raw.githubusercontent.com/mirror/newlib-cygwin/master/newlib/libc/stdio/setvbuf.c).
The scalar MP3 configuration already defines `MINIMP3_ONLY_MP3` and
`MINIMP3_NO_SIMD`, returns S16 directly, and uses integer output interpolation.
Keep those settings and codec provenance. Internal minimp3 synthesis still
uses floating point; S16 output does not make it a fixed-point decoder.
Vorbis short-interleaved output and exclusive decoder ownership stay intact.
Keep read-ahead installed before I/O and alive through fclose. Remove adapter
yields: the reader can decide whether to yield from reservoir occupancy,
whereas an unconditional yield after a cached stdio read cannot.

The pre-change scalar benchmark at -O2 on a synthetic 48 kHz MP3 measured
24.645 microseconds per MPEG frame (1152 output PCM frames). The deterministic
single-CPU model in `tests/host/desktop/music_contention_model.py` uses that
cost, a higher-priority virtual competitor occupying 10 ms of every 20 ms,
and explicit normal-reader redispatch sensitivity. Cost x10/x30 alone does
not explain an empty reservoir. With one normal quantum between dispatches,
the shallow queue advances about 40 s in 60 s while the reservoir stays full.
With three normal quanta after each reader yield, the old 1024-frame batches
advance about 21 s in 60 s and repeatedly empty the reservoir. This models a
scheduling hypothesis, not measured T23 scheduler behavior; an empty ring at
every pump turn is not reproduced (a turn submitting PCM has nonempty input).

Decision: keep the existing 2 ms pump timer, fill the 2048-frame stream when
it falls below 1792 frames, and limit only the first successful submission
to 256 frames. Retain audible-history accounting for exact pause replay.
Decode up to 4096 frames per batch, yield only above half-full, and wake the
reader when audible retirement frees a complete batch. Publishing PCM does
not block or rendezvous with the pump. Existing timed pumping needs no new
kernel readiness feature.

## F1-101 prefill, reservoir target and the kernel producer boost (2026-10-22)

Playback starts after `MUSIC_PREFILL_MS` (1 s) of decoded PCM instead of two
seconds. The player defends a `MUSIC_TARGET_MS` (500 ms) reservoir: below it
the pump keeps the device stream at `MUSIC_DEVICE_BEHIND` (1280 frames), one
period under the kernel's producer mark, so the mixer reports the stream
behind its target lead and the kernel raises the decoder (the thread that
issues `AUDIO_REFILL_READ`) and the pump (the writer) to the device band's
lower edge within the shared 20 %/100 ms producer budget; at the target, or
at the end of the file, the pump fills the 2048-frame stream as before. The
reader still fills the ring when the CPU allows, so the f4-34 storage-stall
tolerance stays. The level record ends with
`boost=<flags> boosts=<n> boost_ms=<n> over=<n>` from `AUDIO_GET_PRODUCER`
(flags 1 behind, 2 boosted, 4 withheld; window deltas). Research, decisions
and replay results: `docs/validation/f1-101-audio-producer-priority.md`.
