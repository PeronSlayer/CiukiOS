# f4-34 — Music: a decoded reservoir of at least two seconds and a reader thread, so playback survives storage waits of hundreds of milliseconds

- **Phase:** F4 (media). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f4-34` from main (after `3357582f`).
- **Authority:** `apps/music/` (`runtime.c`, `player.c`, `player.h`, `decoder.c`, `decoder.h`, `view.c` for the buffer indicator), `tests/host/desktop/` Music tests, `docs/design/f4-audio-media-contract.md` (Music section).
- **Not yours:** the kernel audio device, the stream ABI (`AUDIO_GET_STATUS` layout), the storage layer (f1-82 runs in parallel), Files.

## Evidence (T23, image 82fadce3, `legacy/local/physical/82fadce3/serial.log`, read-only)

- The owner played an MP3 from the internal disk while a copy was running: `[music] open path=/Documents/Desktop/Feeling Good cover.mp3 result=0 ms=297`, `decode format=mp3 rate=48000 channels=2 first_pcm_ms=351`, then "l'audio va stra lentissimo e lagga".
- The kernel transport was healthy the whole time: `[audio] transport running published=24577 retired=24569 halts=0 underruns=0 gap_us=16117 lead_periods=8` (and 25522/25514, 26467/26459 later). The stutter is source starvation: the decoder's file reads waited behind the C: volume lock for 100–740 ms at a time (`[storage] vfs_wait drive=C ms=456 holder=C reason=volume`, 28 such records), and the player keeps almost nothing decoded ahead.
- Code: `runtime.c` has one worker that pumps every 2 ms (`music_player_pump`, decode + write); `decoder.c` reads MP3 input in a 16 KiB buffer refilled when half empty (`MP3_INPUT 16384`, `fread`); f4-31 limited Music's combined software/device prefill to 256 frames (5.3 ms) to cut start-up latency and position error. Any storage wait longer than the kernel stream queue (2048 frames, 43 ms) therefore produces an audible gap; a 456 ms wait produces ten of them per second.
- f1-82 (parallel) shortens the storage waits; Music must still tolerate them: a music player with a 5 ms reservoir is wrong on any machine.

## Required

1. **Two threads with distinct jobs.** A reader/decoder thread fills a decoded PCM reservoir (ring in user memory) of at least 2 s at the output rate (96000 frames at 48 kHz, 384 KiB of 16-bit stereo); it blocks on file reads without affecting playback. A writer path (the existing pump) moves PCM from the reservoir to the kernel stream, keeping the kernel queue at its lead as today (no change to the 256-frame device prefill rule, which concerns the kernel queue, not the reservoir). File reads use a read-ahead of 256 KiB per call (one bulk read instead of 16 KiB refills).
2. **Start and seek.** Playback starts when 100 ms is decoded (or EOF), not when the whole reservoir is full; the reservoir then grows in the background. Seek, next/previous and stop flush the reservoir; position and EOF are computed from frames actually written to the device minus the device's `latency_frames` (the f4-31 definition stays), never from decoded frames. Pause keeps the reservoir.
3. **Starvation is visible, never silent.** When the reservoir empties, the player records `[music] starve ms=%u reservoir_frames=%u` (protected, bounded to one per second) and shows a short "Buffering…" state in the view; it resumes without a click (the device gets silence, not a stale repeat). Count starvations in the status record at close.
4. **Budget.** Decoder CPU on the T23 class (1.1 GHz Pentium III) must stay where it is: measure host `-O2` µs per 1152-sample MP3 frame before/after; the reader thread runs at normal priority and yields between reads. Memory: reservoir ≤ 512 KiB; no change to the Music window's memory footprint test beyond that.
5. **Host evidence.** A test with a slow/stalling VFS model (reads that block 500 ms every second) and a 10 s MP3/WAV/Ogg: before the fix the written stream shows gaps; after, zero gaps, position monotonic, EOF exact. Existing Music tests (open latency, Places, record formats) stay green. `make CAP= apps`, `python3 -B -m unittest discover -s tests/host/desktop -t .`.
6. **Report** `build/codex/f4-34-report.md` with the reservoir numbers, CPU before/after, test results, remaining.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No kernel or ABI change; if the player needs a kernel feature, name it in the report instead of working around it. English UI copy.
