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
