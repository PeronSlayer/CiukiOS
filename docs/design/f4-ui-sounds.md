# F4-12 system event sounds

Research checked on 2026-10-13 before implementation:

- [Kenney Interface Sounds](https://kenney.nl/assets/interface-sounds) identifies
  the existing pinned pack as CC0. Reuse its pinned members and deterministic
  22050 Hz mono S16LE conversion; no new sound dependency.
- [Microsoft sndPlaySound](https://learn.microsoft.com/en-us/previous-versions/ms713269%28v%3Dvs.85%29)
  documents immediate asynchronous return and yielding to an existing sound.
  Ciuki's existing worker already owns file/audio I/O. Keep its bounded command
  queue and two voices; drop bursts rather than delaying UI input.
- [POSIX condition waits](https://pubs.opengroup.org/onlinepubs/9799919799/functions/pthread_cond_clockwait.html)
  release the queue mutex while waiting. Keep playback and device queries outside
  that mutex. Use monotonic event timestamps for the 60 ms volume throttle.

Repository comparison: `sounds.c` has fourteen events and no UI gain;
`sound_runtime.c` has asynchronous playback but no mute/VM policy. The Sounds
page already lists assignments and previews them, but lacks schemes and UI
volume. The tray only submits master changes on drag release. F2-47's `libreg`
is present; `reg_import` commits a branch merge in one durable root transaction.
`audio_vm_active()` already reports pending/busy DOS audio but also revokes
invalid sources and arms wakeups; use a separate read-only snapshot predicate; mixer diagnostics
provide a read-only flags word suitable for exposing that observation.

Implementation decisions within F4-12:

- Preserve existing event numbers and append log-on, device connect/disconnect
  and AutoPlay. Empty recycle stays the existing placeholder; AutoPlay may be
  requested by clients when that action is implemented, without adding an
  AutoPlay launcher here. Detect removable-media transitions after the initial
  snapshot, including replacement generations, for device notifications.
- Install `/System/sounds/scheme.cfg`; accept the former settings file as a
  migration fallback. Overlay typed `Desktop\Sounds` registry preferences,
  with each event's `File` in `Desktop\Sounds\<event>`. Control Panel writes
  the whole sound branch in one import, retaining unrelated Desktop keys.
- Persist independent UI volume (0–100). Scale UI PCM in the service, leaving
  the diagnostic hardware tone at its fixed amplitude. Default/No sounds select
  enabled/default mappings or disabled playback; preview can bypass scheme
  disable, but still respects master mute and VM priority.
- Add `CIUKI_AUDIO_DIAG_VM_ACTIVE` to existing mixer diagnostic flags. The sound
  worker reads mute and VM activity before admitting or pumping UI audio and
  discards suppressed sounds; nothing is replayed when the VM becomes idle.
- Submit each changed tray/keyboard master step asynchronously. Emit feedback
  after successful master job completion; throttle at enqueue and playback to
  prevent delayed bursts. Startup and desktop-ready are distinct events.

Host evidence and limitations are recorded in `build/codex/f4-12-report.md`.
