# F4-06 system sounds

Research checked on 2026-10-12 before implementation:

- Kenney, Interface Sounds 1.0: https://kenney.nl/assets/interface-sounds
  explicitly lists Creative Commons CC0; the preserved License.txt identifies
  Kenney and CC0 1.0 Universal. Reuse the repository's exact archive, verifying
  its pinned SHA-256 before populating build/downloads. This pack is provisional;
  the owner will compose replacements.
- Microsoft RIFF documentation:
  https://learn.microsoft.com/en-us/windows/win32/xaudio2/resource-interchange-file-format--riff-
  specifies chunk sizes excluding headers and WORD padding. Parse bounded RIFF
  chunks, including unknown chunks, and reject truncated or inconsistent PCM.
- Intel AC97 2.2 specification (original specification mirror):
  https://sunsite.icm.edu.pl/Linux/alsa/manuals/intel/ac97r22.pdf
  defines 48 kHz as the reset sample rate. The repository's ac97_codec_prepare
  already verifies the fixed 48 kHz DAC rate and reads vendor registers 7c/7e;
  retain this configuration. The existing mixer resamples 22050 Hz streams.

Implementation decision: mono S16LE 22050 Hz WAV, two desktop voices summed
with saturation before the kernel's existing master gain/mute. A dedicated
SDK pthread owns the nonblocking audio descriptor, preloads bounded assets,
handles scheme reloads and writes short quanta. Event dispatch only queues a
command; it performs no disk reads or blocking audio writes in the compositor.
The player preserves staged samples across partial writes/EAGAIN.
The compiled default event order and filenames match build_sounds.py; custom
assignments accept only WAV basenames inside /system/sounds (no traversal). Idle and
shutdown completion use consumed source frames plus the advertised transport
and drain allowance, without calling the blocking AUDIO_DRAIN in the UI.
Shutdown gives sound completion at most two seconds before the power request.
Hardware Test bypasses the event enable checkbox, retains master volume/mute,
and reports success only after the complete one-second 1 kHz tone is accepted.
A successful acceptance does not measure codec audibility or end-to-end latency.
The lead must measure <50 ms latency and audible output on the T23 and run QEMU.

Desktop protocol v2 additions: DESK_SOUND carries an event uint32 at byte 4;
only menu open/select and error/information/question are accepted from toolkit
clients. DESK_SETTINGS requests 4, 5 and 6 reload the scheme, test one event and
test hardware respectively. These remain restricted to the desktop-launched
Control Panel. Hardware sends a deferred TASK_RESULT with the original serial
only after all 22050 source frames are accepted, or an error/timeout; replies
also match the original window identity. Existing v1/v2 traffic keeps its
encoding. The SDK public desktop header must be rebuilt after this extension.

Empty Trash is currently an explicit no-op: Files has no recycling storage
and deletes permanently. Its existing desktop icon's context menu invokes the
empty_recycle sound without claiming to recover or remove stored files.
Runtime gate scenarios leave audio hooks unset so their framebuffer evidence
and deterministic interactions remain independent of an audio device.

Merge reconciliation checked on 2026-10-12 (f4-06b): retain Display request 3
and assign sound reload/test/hardware to 4/5/6; explicitly dispatch the three
sound requests so a display switch cannot reload the sound scheme. Control
Panel links both display.c and sounds.c and tracks the two asynchronous result
serials independently. The startup sound and display frame commit both remain
after redraw. Keep the audio completion callback inside desk_power_tick, after
program-close processing and before descriptor cleanup and the power request.

Research rechecked before reconciliation:
- POSIX write: https://pubs.opengroup.org/onlinepubs/9699919799/functions/write.html
  permits partial writes and EAGAIN with O_NONBLOCK; preserve the existing
  worker's staged samples and nonblocking compositor callback.
- Linux upstream ALSA timestamping:
  https://www.kernel.org/doc/html/v4.19/sound/designs/timestamping.html
  distinguishes writable capacity from queued playback delay. The existing
  Ciuki worker waits for consumed frames plus transport/drain allowance (or
  its two-second shutdown deadline); acceptance alone is not sound completion.
  This is a cross-check of the existing behavior, not a new ALSA dependency.
