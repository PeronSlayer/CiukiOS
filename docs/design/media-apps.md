# CiukiOS image viewer and music player

## Format and implementation decision

CiukiOS will support BMP, PNG, JPEG and static GIF in the image viewer, and
WAV, MPEG Layer III, Ogg Vorbis and FLAC in the music player. The file formats
are decoded by format-specific code rather than by filename-based rendering:
PNG follows the [W3C PNG Recommendation](https://www.w3.org/TR/png/), GIF follows
the [GIF89a specification](https://www.w3.org/Graphics/GIF/spec-gif89a.txt),
JPEG follows [ITU-T T.81](https://www.itu.int/ITU-T/recommendations/rec.aspx?lang=en&rec=2633),
and BMP uses the documented Windows bitmap headers and row layout
([Microsoft bitmap storage](https://learn.microsoft.com/en-us/windows/win32/gdi/bitmap-storage)).
The viewer reuses the existing `WEBIMG.APP` CAPP PNG/JPEG/GIF decoder and the
desktop's indexed `ui_bitmap` presentation contract; BMP is decoded in the
viewer because the paint application only exposes its own private loader.

WAV uses RIFF chunks ([Microsoft RIFF documentation](https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-)),
FLAC uses [RFC 9639](https://www.rfc-editor.org/rfc/rfc9639.html), Ogg uses
[RFC 3533](https://www.rfc-editor.org/rfc/rfc3533.html) with Vorbis decoding,
and MP3 uses MPEG audio Layer III ([ISO/IEC 11172-3](https://www.iso.org/standard/22412.html)).
The exact decoder sources and licenses are recorded in
[`third_party/audio/README.md`](../../third_party/audio/README.md): `dr_wav`,
`dr_mp3`, and `dr_flac` from pinned dr_libs revision
`dfe8377631000664666519fdb83da193fd8037f4`, plus `stb_vorbis` from pinned stb
revision `2c980bb59875b0d32144a71867fbdebb2f77cd20`.

Music decoding runs in a DJGPP/DPMI worker, following the existing preemptible
`WEBWORK.EXE` mailbox and `VMFORK`/`DPMIRUN` execution pattern. The worker emits
bounded PCM chunks, resamples to 48 kHz stereo signed 16-bit, and supports frame
seek. The desktop app sends those chunks through the shell audio service to the
AC'97 driver. This keeps decoding and playback independent of the GUI paint
path and avoids a full-file PCM allocation. The audio packet ABI is 30 bytes;
its data pointer names the caller's conventional-memory PCM chunk.

## Long-track playback arithmetic and seek state

The worker/audio ABI stores frame totals in unsigned 32-bit values. The C
committee draft specifies that unsigned results are reduced modulo the type's
range ([WG14 N1570, §6.2.5](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf));
therefore `played_frames * 100` can wrap for ordinary multi-hour-length
tracks. The progress bar computes an exact integer percentage by comparing
against quotient/remainder-derived thresholds. Mouse seek uses a quotient and
remainder split with the clamped screen coordinate, so neither calculation
forms a large frame-count product or requires a 64-bit runtime helper.

Seek carries the app's paused bit in the audio packet. The driver discards the
old DMA queue and resets its frame base while retaining that requested pause;
it queues fresh PCM without starting DMA until Resume. This follows the ICH7
Run/Pause register contract, which retains stream state when paused (see
[`ac97-music-streaming.md`](ac97-music-streaming.md)). Decoder command rejection
is treated as an error and closes the paired audio/worker state rather than
leaving the UI waiting for a command that was never accepted.

## Validation scope

The playlist extension check must handle `.flac` before applying the
three-letter-extension guard. Xiph's [FLAC tool documentation](https://www.xiph.org/flac/documentation_tools_flac.html)
uses `.flac` for native FLAC output. The original player returned early when
the fourth character from the end was not a dot, so the Files association
accepted FLAC but the player's sibling playlist omitted it. The four-format
runtime fixture exposed this as Next from Ogg wrapping to WAV. Keep the
case-insensitive four-letter check independent of the WAV/MP3/Ogg guard.

Worker startup failures need a foreground stage and returned AX value in the
serial evidence: short-path resolution, session discovery, XMS allocation/
lock/write, VM enumeration, DOS EXEC and VM status. The existing DOS/XMS and
VM interfaces remain unchanged. Do not infer a memory failure from the generic
"Could not start the decoder" UI message. Log only failed startup operations;
the streaming and drawing loops do not write this diagnostic.

The mailbox input byte count includes the pathname terminator and may be odd.
XMS 3.0 function 0Bh requires an even transfer length; `webstore_write` enforces
that rule. For `C:\\QA\\AUDIO\\D.FLAC`, 19 bytes must therefore be copied using
a zero-padded 20-byte transfer while preserving `input_bytes=19` in the protocol.
Use the existing 260-byte scratch limit; do not relax the XMS mover's checks.
This follows the [XMS 3.0 specification](https://ps-2.kev009.com/basil.holloway/ALL%20PDF/Microsoft_XMS_3%5B1%5D.0_Specification.pdf).

The source and licenses are pinned and hashed. Decoder worker and application
integration are compiled with the full-image toolchain. Runtime codec coverage
belongs in the full HDD profile; the standalone floppy profile is outside the
active project build.

## Viewer follow-up

Viewer decoding runs only from foreground `EV_POLL`: BMP processes at most four
source rows per call, and WEBIMG contributes one decoder step
and one bounded RGB rectangle. In-progress work returns the shell's busy-only
poll result; the final damage update is queued only after the entire cache is
initialized. The completed RGB888 image stays in webstore/XMS; paint only
presents that cache. Idle polling does not redraw the image. Pan uses drag or
arrow keys; wheel selects integer zoom, while Fit returns to the full-image
view.

RGB rectangles are merged into padded XMS rows, so JPEG block rectangles do
not overwrite earlier spans from the same row. Rows untouched by a decoder
(notably GIF logical-screen margins outside a sub-image) are initialized to
black before presentation. BMP scanlines decode into a separate RGB scratch
buffer before merging; this keeps cache assembly from clearing its own source.
The completion flag remains clear until cache finalization finishes, so
multi-poll finalization cannot be skipped by the poll gate.

The Viewer does not register a separate hit rectangle over its viewport:
overlay hit 7 shadowed the window-body hit 70 and prevented body mouse events
from reaching the Viewer. The normal body hit 70 routes wheel and drag input;
keyboard and toolbar `+`/`-` zoom remain available when a host does not deliver
wheel events. Event-only serial markers record delivered keys, wheel direction,
zoom changes, viewport entry/exit and pan begin/end without logging every
pointer-motion sample.

BMP support is limited to uncompressed 8-bit indexed, 24-bit RGB and 32-bit
RGB `BITMAPINFOHEADER`-compatible files. The parser checks one plane, BI_RGB,
positive signed width, nonzero signed height (including top-down images),
palette placement after the declared DIB header, and the complete pixel array
against the actual DOS file length. It accepts dimensions through 2048 by 2048
and a maximum 16 MiB padded RGB888 cache; larger images are rejected. Previous
and Next inspect at most 32 matching siblings, including `.jpeg` files.

These checks follow Microsoft's bitmap storage description linked above: DIB
height is signed, negative height denotes top-down rows, and pixel data begins
at the file header's `bfOffBits`; palette placement depends on the DIB header
size. WEBIMG remains the existing decoder boundary for PNG, JPEG and static
GIF. This keeps file reads and codec work out of `EV_PAINT`, so compositor
repaints never perform DOS I/O or advance decoder state.

## Deferred player window close

The later CPU-capped runtime completed all four codecs, but its explicit close
of an active Ogg track emitted `Error` after the worker ended. `EV_POLL` still
called `poll_worker` while `closing` was set, treating the deliberately stopped
worker's end as a failed decode. While closing, Player now advances only
`mediawork_close`; decode/audio polling resumes only for an active stream. This
also protects an in-flight Next/Previous transition from canceling its pending
replacement track. The mailbox remains retained until the existing VM-status
confirmation proves teardown complete.

Microsoft's [asynchronous cancellation guidance](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations)
distinguishes expected cancellation completion from operation failure and
requires pending resources to remain valid until completion. This is a lifetime
principle applied to CiukiOS's own worker protocol; Player does not call Win32
I/O APIs.

The all-format recorder run completed WAV, MP3, Ogg and FLAC plus Previous,
then exposed an actual lifecycle bug: `EV_CLOSE` vetoed immediate destruction
while the decoder was stopping, but `EV_POLL` never retried the close after
`mediawork_close` confirmed termination. The Player remained visible with stale
Playing text and blocked the harness from reopening through Files.

Keep a separate requested-window-close flag from track-change/Stop teardown.
After mailbox release, queue service 14 command 2 for the Player window itself;
the shell delivers that identified close only after the CAPP callback returns.
The existing `app_s_window_cmd` records `app_close_window`, and `app_after`
already raises and closes that recorded target for input callbacks. Extend the
end of `app_poll` to deliver the same queue after all callbacks have returned.
Do not infer the target from whichever window has focus at that later time.

[Intel's CALL/RET instruction reference](https://cdrdv2-public.intel.com/789581/325383-sdm-vol-2abcd.pdf)
describes far calls and returns through the code segment and instruction pointer.
The CAPP callback must return to the shell before its executable allocation is
freed; this local deferred queue enforces that lifetime. The shell owns the
window and native module cleanup, rather than unloading executable code from
inside its own callback. Browser's existing deferred teardown provides a local
precedent, but the Player queue explicitly retains the window ID.

The close-request serial marker precedes the CAPP veto and is not proof of
window destruction. The runtime harness must wait for both window 20 flags to
be zero and Player slot 26 to be unloaded before typing into Files again.
This closure correction is separate from earlier Jemm #06 captures; a passing
production rerun is still required.

## Opt-in CAPP service-return trace

The `APP_RETURN_TRACE` diagnostic records the CAPP far-return frame at the
shell service boundary. Intel's [far CALL/RET specification](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366521.pdf)
defines a far call as saving CS and the return instruction pointer on the
caller stack, with the return IP at SS:SP and CS immediately after it. In this
repository, `app_start.asm:svc_` makes that far call through the CAPP header,
and `shell_apps.inc:app_service` saves the module SS:SP before switching to the
shell stack and restores it before `RETF`. The diagnostic snapshots those
saved stack words, expected caller segment, service and slot, and the shell
stack location immediately before that return. Its `CAPPRET1` header is
followed by a 16-record ring of 20-byte records containing ten 16-bit fields
(sequence, module SS:SP, expected caller, return IP:CS, service, slot, and shell
SS:SP); a CS mismatch is recorded and
freezes the ring without stopping execution. The trace performs no DOS or
serial I/O and preserves registers and flags. It is compiled only for the
explicit `qemu_test_music_player.py --app-return-trace` run, which installs
that instrumented shell into its disposable disk copy; ordinary shell builds
and the source image remain unchanged. This evidence can establish the frame
contents at the service return boundary, but does not alone identify who
modified an invalid frame earlier or later.
