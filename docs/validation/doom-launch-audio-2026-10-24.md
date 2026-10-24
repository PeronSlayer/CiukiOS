# DOOM launch audio gaps and boot sound gaps, image 9fe0a02e (QEMU)

Host analysis, 2026-10-24, worktree `wt/daudio`. Evidence from the lead's QEMU
KVM run of the image of main `9fe0a02e` (`build/test-runs/doom-audio/` in the
main checkout, not tracked): `doom.wav` (QEMU `wav` audiodev, 16-bit stereo
44.1 kHz), `serial.log` (cited `Lnnnn`), `replies.log`. Kernel addresses were
resolved against the matching `VMM.ELF` (build identity `9fe0a02e0af2`). No
QEMU, image build or hardware run was made for this record.

## Reading the capture

QEMU's AC'97 model hands the audio backend data only while the guest's bus
master runs ([hw/audio/ac97.c](https://gitlab.com/qemu-project/qemu/-/blob/v9.2.0/hw/audio/ac97.c),
`transfer_audio`, `write_bup` for a halted but enabled channel). The WAV
therefore skips every interval in which the transport is stopped: the boot
sound (L400) and the next window sound (L1426) are 15 two-second cpustat
windows (about 30 s) apart in the serial and 1.1 s apart in the WAV, while
the uninterrupted transport run of the timed demo (L2495 to L7351, 61
windows, about 122 s) fills 121 s of WAV. Each transport session ends with `AUDIO_IDLE_TAIL_MS` = 500 ms of
published silence (`src/kernel/include/ciuki/audio.h:31`,
`src/kernel/audio/audio_dev.c` idle-stop) before the bus master stops.

Each burst was identified by normalized cross-correlation against the
installed sound files (`build/external/system-sounds/*.wav`, 22.05 kHz,
resampled to 44.1 kHz):

| WAV burst (s) | sound (r) | silence after | transport session |
| --- | --- | --- | --- |
| 0.06-0.59 | `startup` 0.74 + `logon` 0.64 (played together) | 0.60-1.15 | L400 start, L448 underrun, L451 idle-stop |
| 1.16-1.55 | `window_open` 0.90 | 1.55-2.05 | L1426 start, L1473 underrun, L1476 idle-stop |
| 2.08-2.45 | `window_open` 0.69 | 2.45-3.95 | L1583 start, L1692 idle-stop, then the DOS Box VM (L2361) |
| 125.57-125.89 | `window_close` 0.95 (timed-demo DOS Box closed) | 125.89-126.45 | L7613 start, L7659 underrun, L7663 idle-stop |
| 126.50-126.65 | `menu_open` 0.85 (Start) | 126.65-127.22 | L7731-L7779 |
| 127.22-127.38 | `menu_open` 0.88 (Programs) | 127.38-127.93 | L7910-L7956 |
| 127.94-128.09 | `menu_open` 0.94 (Games) | 128.09-128.64 | (one session's records are not in the serial) |
| 128.65 / 128.75-129.07 | `menu_select` 1.00 (DOOM), `window_open` 0.97 | 129.07-130.51 | L8023 start, L8145-L8148 tail underrun, L8153 idle-stop |
| 130.51- | DOOM (OPL title music) | none for 60 s | L8659 start ... L8718 recover-start, then `running` |

So the "four bursts of about 200 ms separated by about 550 ms of digital
silence" are the Start, Programs and Games menu sounds and the DOOM click and
window sound, each followed by its 500 ms idle tail; the boot "gaps" are the
startup/logon sound and two window sounds with their tails. They are not gaps
inside one sound and not DOOM audio. The 1.44 s before DOOM's music is the
window sound's tail plus the VM source's silent start while DMX initialises
(start/source-stop pairs L8659-L8663, the same sequence as the timed demo's
L2361-L2371). DOOM's own audio is continuous from its first note (the lead's
analysis found no dropout in the 60 s that follow).

## The hypotheses checked

- Hidden window, VM class and audio source: the desktop focuses the DOS Box
  window at `DESK_CREATE_WINDOW` (`apps/desktop/client.c`, `desk_focus` after
  CREATE), the DOS Box forwards that focus to the VM right after
  `CIUKI_DOSVM_CREATE` (`apps/dosbox/dosbox.c`, "focus that arrived during the
  check"), and the scheduler's focus preference (`dosvm_task_focus`,
  `src/kernel/vm/dosvm.c`) and the audio source registration are the same in
  both launches: identical transport sequences (above) and comparable first
  cadence windows (L2710 timed demo: `late_acks=5 merged=30 ack_max_ms=131`;
  L8872 Start menu: `late_acks=3 merged=17 ack_max_ms=89`). The only
  difference, `underruns=246` then 47 VM output frames (5 ms and 1 ms in
  5 s), is DMX's start-up and is not audible as a gap.
- Print spooler: real and severe, but not the cause of these silences. See
  below.

## Defect found: the print spooler never slept

`apps/printers/spool.c` (G31, merged 2026-10-24 03:46) slept between idle
passes with `nanosleep({0, r>0 ? 1000000 : 1000000000})`. A `tv_nsec` of
1,000,000,000 is invalid ([POSIX nanosleep](https://pubs.opengroup.org/onlinepubs/9799919799/functions/nanosleep.html));
the kernel returns `EINVAL` at once (`src/kernel/proc/clock.c`,
`file_sleep_deadline`), so the idle pass ran back to back. The fdos fix
(`bdb8f811`) made each pass one folder read instead of 257 opens but kept
the loop. Serial evidence on this image:

- `[cpu] top=printspo#43:...` first in almost every window without a game:
  656-767 permille of the CPU, 583-684 of it in the kernel (L825-L1409,
  L2005-L2342, L7990-L8645).
- `[storage] vfs_hold drive=C task=printspool#43`: 100 holds over 50 ms
  (readdir 69, mean 112 ms, max 421 ms; stat 24, mean 94 ms; lookup 7).
- `[kernel] stretch_max_ms=70-108 task=printspool#43` with `eip` all over the
  FAT/VFS path (`next_entry`, cache `find`, `path_upper`,
  `diagnostics_publish`, `memcpy`), and inheritance chains through it:
  `inherit from=storage-writer#15 to=printspool#43 level=24 reason=drive`,
  `inherit from=printspool#43 to=dosvm#58 level=15 reason=cache` (L8572-L8776).
- While DOOM loaded, unfocused work and the VM's DOS file services queued
  behind it (L8382-L8645: printspool 385-747 permille, dosvm 7-235).

Once DOOM runs focused the spooler only gets starvation slices, which is why
the game itself sounded continuous in QEMU. On the T23, where the same path
is several times slower and DOOM's WAD reads are synchronous VMM file
services that deliver no virtual interrupts while blocked (fx-16 root cause
1), a spooler holding C: in a loop would starve DOOM's sound. No T23 trial so
far ran an image with the spooler.

Fix: the event-driven worker of `docs/design/printing.md`, amendment
2026-10-24 (doorbell from libprint, the DOS capture and the Win32 MoveFile
service; retry timer only for Offline jobs; a 60 s idle rescan as the safety
net for a producer that does not ring).

| host test | before (`b5ef0431` spool.c) | after |
| --- | --- | --- |
| `print_spool_wait_test.c`, idle folder reads (rescan scaled to 1 s) | 209,116 in 1 s, FAIL | 2 in 2.5 s |
| job published without a ring | (busy loop) | printed by the next rescan |
| submitted job printed | (busy loop) | 2-6 ms |
| job published between the empty scan and the sleep | - | printed at once |
| offline job, 300 ms test interval | - | 4-5 reads/s, printed 202-229 ms after its port appeared |
| `dos_print_test.c`, DOS job rings the worker | no ring, FAIL | 1 ring per publication |

## Why the earlier audio fixes did not cover this report

- fx-16 (VM monitor costs, FAT leading-run cache, IRQ0 reinjection, frozen
  devices while parked), f4-31/f4-32 (transport lead 8, adaptive growth to
  28 periods), f4-24 (mixer cost) and f1-101 (native producer boost) act on
  the DOS VM, the transport and native players. The silences here are
  neither: they are idle tails between separate UI sounds, visible only
  because the WAV capture skips stopped intervals. Measuring them as gaps
  would have led to changing a component that is working.
- `bdb8f811` reduced the spooler's work per pass but did not notice that the
  pass never slept; the spooler is a native background process outside all
  audio paths, which is why none of the audio fixes could address it.

## Underruns at the start of a session (lead decision)

Every transport session started with an 8-period lead (42.7 ms) because
idle-stop reset `adaptive_lead` and an underrun did not grow it, although
the measured service gap persisted (`gap_us=133577`). A stall in the first
periods underran: holes of 30 ms in the startup sound (WAV 0.31-0.33, L448)
and 90 ms in `window_close` (WAV 125.49-125.58, L7659); the window sound at
L1473 and DOOM's start (L8716, before its music) underran too. The lead
decided to keep the grown lead after the idle stop (30 s, then a decay to
the nominal lead by 80 s) and to grow it on an underrun: contract amendment
in `docs/design/f4-audio-media-contract.md`, replay
`tests/host/audio/lead_hold_test.inc` (pre-decision code: L1426, L7613 and
L8659 sessions start with 8 periods and underrun once each; after: 28
periods, no underrun; the first sound after boot keeps the nominal lead).

## Also visible, not changed here

- The 70-130 ms stretches recur in QEMU with every task (dosvm at `sti` in
  `dosvm_trap`, `storage-io` in `native_outsw`, desktop in `row_copy`,
  `audio-refill` in `ac97 write_io`); the PIIX3 ATA runs PIO in QEMU
  (`reason=piix3_no_udma`). They are QEMU timing, not T23 evidence.
- The spooler still persists job progress with an fsync after every 1 KiB
  while printing (unchanged G31 durability contract).
