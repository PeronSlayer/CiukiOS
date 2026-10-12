# f4-06 — System sounds (provisional CC0 pack), sound events and an audible hardware test

- **Model/effort:** `gpt-6.1-sol`, `high`, `--search` allowed for the pack research. **Worktree:** `../CiukiOS-wt-f4-06` from `main`. Host-testable; the lead verifies on the T23 (AC97) and QEMU.

## Owner request (2026-10-12, T23 test)

"L'audio non si sente nulla, mancano proprio feedback sonori: scarica un pack di suoni gratuito per sistemi operativi e includilo e sistemalo, trattalo provvisoriamente perché in futuro li comporrò da solo i suoni."

## Scope

1. **Pack.** Choose a Public Domain / CC0 UI sound pack (for example Kenney's "UI Audio" or "Interface Sounds", CC0 1.0; verify the licence on the source page and record the URL, author and licence in `assets/sounds/README.md` and `LICENSE.txt`, and ship the notice as `/system/sounds/CREDITS.TXT`). Download with pinned SHA-256 into `build/downloads` like the other external assets, convert deterministically to 16-bit PCM WAV mono 22050 Hz (or the rate the AC97 path uses natively) with a script `scripts/build_sounds.py`, and install to `/system/sounds/<event>.wav`. Keep the total under 1 MiB. Mark the set provisional (the owner will compose his own).
2. **Events** (desktop): startup (after the first frame), shutdown (before the power request, waited for completion, at most 2 s), menu open, menu select, window open/close, minimise/restore, error, information, question dialogs, Task Manager End Task, volume slider move (Control Panel), empty recycle. Map each event to a file through a scheme file `/system/settings/sounds.cfg` (event=file, enabled=0/1) with the defaults compiled in.
3. **Playback.** A small mixer-fed player in the desktop (or the existing audio path of the SDK: find how `apps/accessories/mixer` and the DOS VM source reach `/dev/audio` or the mixer) that plays a WAV asynchronously without blocking the compositor (a thread or non-blocking writes), two overlapping sounds at most, honouring the master volume and mute. Latency target < 50 ms on the T23.
4. **Control Panel > Sounds**: event list with the assigned sound, Test button, "Play system sounds" checkbox, and a **"Test audio hardware"** button that plays a 1 kHz tone for one second and reports whether the device accepted the data (so the owner can tell a silent codec from missing events). Kernel: a verbose-boot record of the AC97 codec id, rate and the first playback (`[audio] codec=.. rate=.. play=ok`) if not already present.
5. Tests: scheme parsing, event→file resolution, player state machine and mixing arithmetic on the host; Control Panel page render.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f4-06-report.md` with the pack provenance.
