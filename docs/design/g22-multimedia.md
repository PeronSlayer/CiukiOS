# G22 multimedia completion

Research and implementation contract, 2026-10-23. Scope: inventory MM-1,
MM-3–MM-7, MM-9 and AX-9. Hardware qualification belongs to the lead.

## Reference behaviour

The owner's Windows 98 Resource Kit, read at
`/home/peronslayer/Desktop/CiukiOS/build/rskt`, describes device-independent
MCI transport in `wrkc12_1.htm`, waveform/MIDI playback and Sound Recorder
in `wrkc12_2.htm`, video playback in `wrkc12_3.htm`, and separate Sounds and
Multimedia applets in `wrkc23_2.htm`. Sounds creates and modifies schemes;
Multimedia selects playback, recording and MIDI devices and CD volume.
Volume Control exposes the lines actually supplied by the selected driver,
with separate playback and recording destinations.

Primary online references:

- [Microsoft: Audio Mixers](https://learn.microsoft.com/en-us/windows/win32/multimedia/audio-mixers)
- [Microsoft: Recording Waveform Audio](https://learn.microsoft.com/en-us/windows/win32/multimedia/recording-waveform-audio)
- [Microsoft: MCI](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci)
- [Microsoft: MCI_STATUS](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-status)
- [Microsoft: MCI_PLAY](https://learn.microsoft.com/en-us/windows/win32/multimedia/mci-play)

## Repository comparison and decisions

The existing sound worker loads bounded PCM WAV clips; preserve its format
validation, voice limits and asynchronous playback. Browse must validate the
selected file before changing the saved assignment. Preserve Default and No
sounds, add named schemes, and disable one event with None. A failed save
must retain the previous active scheme. Named schemes use bounded, validated
names and atomic replacement; built-in schemes cannot be overwritten/deleted.

The tray's single click keeps the small master popup; a second click within
the configured interval closes that popup and opens Volume Control. It must
invalidate the old popup footprint, including on cached presenters.

The kernel has a copied, exclusive private capture engine (`audio/voice.c`)
for a subset of driver families. Public capture must retain its generation,
bounded-copy, stop-proof and quarantine rules, automatically release capture
on descriptor close, and report unsupported devices truthfully. A recording
overrun is an error, never a fabricated silent recording. Playback and capture
controls must not introduce a panic path. Any unsupported driver family or
unconnected MCI/native bridge remains explicitly listed in the inventory
and implementation report rather than being counted as completed.

## Implemented interfaces and bounds

Capture appends devctl operations `0x4109`–`0x410c` after the former highest
`AUDIO_GET_PRODUCER` (`0x4108`). No syscall or desktop message is added.
The 40-byte `ciuki_audio_capture` and 536-byte `ciuki_audio_capture_data`
records have explicit version, size and reserved-field validation. Open
`/dev/audio` read-only, query CAPS, then explicitly START, READ (up to 128
stereo frames per call), STOP. Capture is exclusive across descriptors;
close also stops it. A read-only capture descriptor rejects playback writes
before dereferencing a stream. Playback descriptors reject capture requests.
All sample data crosses the existing pinned, cancellable copied boundary.
The driver retains DMA pages when stop proof fails, as before G22.

The existing `capture_ops` are installed by Trident DX/NX and ALi M5451.
Those families expose this public capture path. Other private capture
implementations (including EMU10K/CMI) do not implement that same interface;
they are not silently advertised. The T23's ICH/AC97 driver does not yet
provide it. Microphone selects AC97 record mux input 0, line input 4 in
both channels, and the original mux/gain are restored at stop. The original
Linux [AC97 codec implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/jic23/iio/%2B/1da177e4c3f41524e886b7f1b8a0c1fc7321cac2/sound/oss/ac97_codec.c)
corroborates those record-select values. Gain 0–100 is attenuation of copied
samples, not a claim of analog microphone boost.

Sound Recorder holds at most 60 seconds of S16LE stereo 48 kHz PCM
(11,520,000 bytes). Playback retains its position on short writes and
EAGAIN/EINTR and drains the last queued frames. Record appends; Delete before
and Delete after trim at the position slider. Reverse and volume effects
preserve stereo pairing and clamp overflow. Save As writes a PCM WAV through
an exclusive temporary file and rename; failure keeps the unsaved document.
Open currently accepts that same canonical PCM WAV encoding only. Close and
session shutdown use the toolkit's save/discard/cancel handshake.

Sound schemes retain the existing 22050 Hz mono S16LE, two-second clip
format and 63-character assignment bound. Up to 16 named schemes with
31-character names can be saved, loaded and deleted. Default, No sounds and
Custom are reserved. Named scheme changes do not alter the active scheme
until its separate registry save succeeds. The Multimedia page reports the
single current system device, capture availability and existing Win32/DOS
synthesizers; it offers Volume Control, Recorder and Music Player. Alternate
preferred-device routing and CD mixer controls remain gaps.

The CD Player still uses the Win32 personality's public MCI. It now lists
tracks and lengths, displays position, skips data tracks, bounds PLAY to one
audio track, advances on successful completion, supports random playback
without repeats and optional disc repeat. Refresh reloads a changed disc.
Disc title/track-name database and a native MIDI/AVI MCI bridge remain gaps.
The latter must retain MCI's file-identity ownership, worker/thread rules,
AVI rendering and audio transport; the current engine depends on Win32
handles and USER/GDI and cannot simply be linked into a libdeskui client.

## G22B continuation contract (2026-10-24)

The second-parts directive retains the Windows 98 Resource Kit behaviour above.
The local chapters `wrkc12_1.htm`, `wrkc12_2.htm`, `wrkc12_3.htm` and
`wrkc23_2.htm` were checked again: playback/recording lines are separate;
Multimedia chooses among installed devices, rather than inventing devices;
MCI is the shared transport for waveform, sequencer, CD and video playback.

Additional primary references checked before implementation:

- [Microsoft: managing waveform recording](https://learn.microsoft.com/en-us/windows/win32/multimedia/managing-waveform-audio-recording):
  prepared application buffers, queued input, completion callbacks, Stop and Reset.
- [Microsoft: waveInOpen](https://learn.microsoft.com/en-us/previous-versions/dd743847(v=vs.85)):
  format query does not open hardware; mapper, callback types and error results.
- [Linux v6.12 intel8x0.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/intel8x0.c):
  independent PCM-input register block at offset 00h, BDL/CIV/LVI/PICB,
  channel RUN/reset and FIFO status; AC97 record selection remains in the codec.

Repository comparison: ICH PCM-out uses a separate transport from the cyclic
voice drivers. Its PCM-input adapter must use the existing exclusive copied
capture lifetime, with its own descriptor page and pinned DMA pages. It must
prove RUN clear, DCH and a stationary cursor before freeing pages. Unsupported
controller layouts stay unavailable until their independent capture register
model passes; no silent samples or speculative DMA programming. Capture errors
return existing CiukiOS errors; no new panic paths.

Mixer source controls must alter the actual signal route: software Wave/MIDI/CD
attenuation belongs before the master sum, analog CD/Line/Mic controls belong to
the codec, and Recording selects one input. The desktop remains the delegated
mixer writer. Any capability not delivered is retained as an explicit inventory
gap, with its technical reason and verification status in the G22B report.

### Continuation implementation and limits

G22B appends `AUDIO_SET_SOURCE=0x410d`, `AUDIO_CAPTURE_ACQUIRE=0x410e`,
`AUDIO_CAPTURE_RELEASE=0x410f`, `MIXER_GET_LINE=0x4205` and
`MIXER_SET_LINE=0x4206`. The previous highest operations in all 77 visible
local/remote refs were 0x410c and 0x4204. No syscall or desktop opcode is added.
`DESK_MIXER_SET` retains its original 20-byte master request and accepts a
28-byte source request (append line and recording destination). Authorisation
still comes from the desktop's delegated mixer client identity.
The 16-byte audio-route and 40-byte mixer-line records have a reserved final
word and an oracle entry for every field and constant.

The native Volume Control now has Master, Wave, MIDI, CD Audio, Line In and
Microphone playback columns. Recording shows Stereo Mix, CD, Line and Mic
when supplied by the capture driver, with an exclusive Select control. The
recording gain is copied-sample attenuation, not microphone boost; recording
balance and mono microphone balance are disabled. The complete window fits a
640-pixel display. Source preferences are atomic stereo/mute values in
`System\Audio` (`Source1`…`Source5`); `Recording` stores the selected source
and gain. They are restored at boot. Analog controls preserve unrelated bits
(including microphone boost), save their original register values before the
first write and restore them during safe backend shutdown. Wave, MIDI and CD
attenuation happens before the master sum. Native streams can declare their
source; WinMM marks its mixed stream and applies separate Wave/MIDI/CD gains,
including MCI CD playback. DOS PCM/OPL respect Wave/MIDI respectively.

The AC97 register identities and mux values were cross-checked against the
[Linux AC97 register definitions](https://android.googlesource.com/kernel/common/+/ff729a0610eb/include/sound/ac97/regs.h)
and the upstream [AC97 mixer implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/69050f8d6d075dc01af7a5f2f550a8067510366f/sound/pci/ac97/ac97_codec.c).
CD is mux 1, Line 4 and stereo Mix 5 in both channels; Mic remains 0.

ICH capture owns eight PCM pages plus a descriptor page. Data and descriptor
pins have separate FROM_DEVICE and TO_DEVICE directions. Input polling keeps
its interrupts disabled, refreshes LVI behind CIV, checks FIFO/cursor/time
bounds, and never resets the output channel. Input stop proves RUN clear,
DCH and a stable cursor before unpin/free; failed proof quarantines all DMA
ownership. The ADC rate and record mux/gain are restored. The standard ICH
layout is exposed; SiS and ALi M5455 extensions remain unavailable. Trident
DX/NX and ALi M5451 keep their existing copied capture path.

Win32 waveIn now supports enumeration/caps, format query, exclusive open,
prepared/queued buffers, mono/stereo 8/16-bit PCM at 4–48 kHz, Start/Stop/Reset,
position and function/window/thread/event callbacks. It converts the copied
48-kHz input with bounded decimation, discards input when no buffers are
queued, returns partial buffers on Stop and all queued buffers on Reset,
and rejects Close while buffers remain queued. Open reserves input without
starting DMA; Stop retains that reservation; the last descriptor close
releases it. The existing WinMM worker delivers callbacks outside its audio
lock. Capture faults stop input rather than returning invented silence.
[waveInStop](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveinstop)
and [waveInReset](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-waveinreset)
were checked for buffer-return and position semantics.

The Multimedia page now also changes CD volume through the same delegated
source request. It still describes the one selected system device; alternate
playback/recording/MIDI device selection is not implemented.

CD Player now edits artist, album title and track names. It identifies a disc
from every track's position, length and audio/data type, clears names before
refresh, and saves one complete INI section to `C:\System\cdplayer.ini` using
[WritePrivateProfileSection](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-writeprivateprofilesectiona).
The two TOC hashes are a local database key, not an assertion of Windows INI
file compatibility. Failed saves retain the edits in the window. Names and
section size are bounded and control characters are refused. MCI retains CD
transport ownership; no disc metadata is inferred from raw audio.

Remaining obligations: alternate device selection, analog recording boost,
non-AC97 source controls, independent per-input recording gain, native MIDI
and AVI Media Player over MCI, native MIDI port, DOS MPU-401 UART, and public
capture adapters for the other families. The existing CMI and EMU private
capture interfaces have different ownership/read/stop rules (EMU stop also
stops playback); they have not been advertised through the voice interface.
No unsupported family is counted as delivered. Hardware evidence and an
unrestricted LeakSanitizer rerun remain the lead's integration work. Exact
commands, results and the pre-existing compositor fuzz failure are recorded
in `build/codex/g22b-report.md`.

The desktop mixer keeps a complete pending snapshot in the existing 64-byte
settings-worker job slot. Master and separate source changes survive queue
coalescing; repeated changes replace only that control's value. Recording
coalesces its last mux selection and newest shared gain in chronological order.
A compile-time capacity assertion and `mixer_queue_test.c` exercise this boundary
and rejected submissions. No worker buffer or stack limit was enlarged.
ICH startup waits for DCH to clear before the first public READ; a stuck start
has a bounded timeout and proved cleanup. Codec profiles with independent
stereo mutes update both mute bits and restore the original source registers.
