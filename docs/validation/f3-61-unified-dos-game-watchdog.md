# f3-61 unified CiukiDOS: a game in full screen, the watchdog and data loss

Recorded 2026-10-22 09:41 CEST (worktree `wt/f3-61`). Host evidence only:
the production DOS VM code on libx86emu (`tests/host/dosvm_guest.c`, H50
hook-CPU build) and the host suites. No QEMU run, no OS image, no hardware.
The lead owns the QEMU case `f3:vm-game` and the T23 confirmation.

## Sources

| Topic | Source |
| --- | --- |
| T23 trial | image b453bc92, `build/codex/t23-b453bc92-serial.txt` and `t23-b453bc92-BOOT.LOG` (VM 1 generation 5469, DOOM, `CiukiDOS unified (default)`) |
| VM lifecycle | `src/kernel/vm/dosvm.c` (`dosvm_destroy`: `dosvm_stop(key, forced ? DOSVM_TERMINATE : DOSVM_EXIT)`, `data_loss = dirty && (forced \|\| reason != EXIT)`); `dosvm_client.c` `release_binding` (forced = VM still running) |
| Full-screen return | `src/kernel/vm/dosvm_fullscreen.c` (`fullscreen_return` is set only by Alt+Enter/Alt+Tab, `dosvm_fullscreen_stopping`, a VBE flush error and a VBE mode change) |
| DFV2 dispatch | `src/kernel/vm/dos_file_vm.c` `dosvm_file_interrupt`; `dosvm.c`/`dosvm_client.c` set `disable_file_bridge` for every kernel other than CiukiDOS |
| Fork drive letters | K2043 `kernel/initdisk.c` 157–200, 825–850, 1316–1326: C: is the first bootable primary FAT partition (`SCAN_PRIMARYBOOT`), else the first primary FAT partition (`SCAN_PRIMARY`); FAT types 01h, 04h, 06h, 0Bh, 0Ch, 0Eh (`IsFATPartition`) |
| Canonical DOS bases | `scripts/dos_conformance.py` `build_reference`: MBR + FAT16 at LBA 63 (`REFERENCE_START`), `.SHA` sidecar |
| Windows 9x close | RBIL INT 2Fh/168Fh [DH=00h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4631.htm) (enable/disable the system-menu Close command) and [DH=01h](https://mirror.cs.msu.ru/oldlinux.org/Linux.old/docs/interrupts/int-html/rb-4632.htm) (query close); PIF Misc "Warn if still active" ([PIF overview](https://en.wikipedia.org/wiki/Program_information_file)); Win9x COMMAND.COM is close-aware, so an MS-DOS Prompt waiting at its prompt closes without the warning, a running program does not (VOGONS [thread](https://www.vogons.org/viewtopic.php?p=1173225), quoted from the search index; the page itself refused retrieval, HTTP 403) |

## What the T23 log shows

- No `[dosvm] DFV2 IFS active`, `DFV2 bind refused` or
  `unified_activation_refused` record exists for the DOOM VM. The fork ran
  with its own local FAT over BIOS INT 13h (see below).
- `fullscreen_request mode=19` at tick 205240, first frame (aperture write)
  at 206468, then 41 s of full screen with protected-mode activity, PIT and
  OPL (ports 388h/389h) exits: the game loop and its music were running.
- `fullscreen_watchdog reason=return-or-fault` at 247235. Mode 19 is the
  VGA path (no VBE flush or VBE mode change), and the VM did not stop (it
  logged `fullscreen=0 … suspended=0` and seven more perf records); the
  remaining producer of that reason is the Alt+Enter/Alt+Tab hotkey.
- After the return the protected-mode counts fall to 13160, 458, then 0
  with `idle_28` records: DOOM ended within 2–4 s and the shell waited at its
  prompt for about 13 s.
- `lifecycle stage=stop reason=1` is `DOSVM_TERMINATE`, which only
  `dosvm_destroy(forced)` produces, that is `release_binding` of a VM still
  running: the window was closed while the shell was idle. `data_loss=1`
  is that forced close of a VM whose private overlay was dirty. The watchdog
  did not stop the VM. Why DOOM ended after the return is not in the logs (no
  VM console capture); the `f3:vm-game` case records it.

## Root cause 1: `kernel=unified` never bound in a real VM

`dosvm_file_interrupt` returned `-ENOSYS` when `disable_file_bridge` was set
before it reached the DFV2 dispatch added by f3-56/f3-57, and every
non-CiukiDOS kernel (including `kernel=unified`, schema value 2) is created
with that flag. CIUKIDF.SYS's discovery INT 2Fh was never answered; it
printed `CIUKIDF: no DFV2 host; driver not installed` and FreeDOS stayed
unbound. The H57 CPU harness calls `dos_unified_gate` directly, so it never
crossed this function; the QEMU conformance probe also passes unbound (it
only checks that the v1 bridge is absent). Decision: the DFV2 gates are
served before the v1 refusal (`v->kernel==2` first); the v1 bridge stays
refused for every FreeDOS kernel.

## Root cause 2: the native mount took the MBR as the FAT volume

With the gates served, the production `unified_mount` mounted the VM disk
provider from LBA 0, the MBR of the partitioned base, and the activation was
refused (`unified_activation_refused reason=native_mount_failed`): every
unified launch would have stopped before the shell. The H57 test host offsets
its provider by 63 sectors itself, so it never saw this. Decision: the mount
scans the VM disk with the existing `partition_scan` and mounts an offset-only
`blkpart` view of the partition FreeDOS assigns to C: (first bootable primary
FAT partition, else the first primary FAT partition); a disk without a valid
partition table keeps the whole-disk view. Guest INT 13h inside that partition
is serialized through the logical view at `lba - start`; sectors outside it
(the MBR and the gap) return `-ENOSYS` from `dosvm_file_disk`, and
`dosvm_disk_read/write` take the plain path for them.

## DOOM: freedos-1.3, the fork unbound and the fork with the live IFS

Production VM on libx86emu, canonical bases of image 8dfe84a3…
(`build/f0/dos/FREEDOS.IMG`, `UNIFIED.IMG`, read only), DOOM shareware 1.9,
`DOOM.EXE -config CIUKI.CFG`, default sound (SB16 220/7/1 + OPL):

| Run | freedos-1.3 | fork unbound (T23 state) | fork + live IFS |
| --- | --- | --- | --- |
| Boot | prompt, free 7CC4h paragraphs | prompt, 7C45h, `no DFV2 host` | prompt, 7BD2h, `DFV2 IFS active drives=4`, 124 IFS requests, 14 opens = 14 closes |
| INT 21h trace from DOOM's EXEC, 40 s | 2306 calls | identical after normalization | identical prefix (2306), then 25 more lump reads: the IFS run is faster |
| WAD access | 3Dh open, 42h SET seeks, 3Fh reads of 2000h bytes through DOS/4GW's 0302h calls into the INT 21h vector | same | same; 4 DOS errors, all expected (43h on doom.wad/doom2.wad/doom2f.wad, DOOM.ETX) |
| Attract loop, 150 s | mode 13h Mode Y, 417 flips, no exit | 417 flips, no exit | 451 flips, 1282 IFS reads (3.1 MB), no critical error, no deadline expiry |
| Quit through the menu | ENDOOM, prompt | — | ENDOOM, DEFAULT.CFG written through the IFS (2 writes, 816 bytes), 20 opens = 20 closes, prompt |
| `-timedemo demo1`, 900 s simulated | 3435 frames, still running | — | 3480 frames, still running, no IFS critical error |

Runtime differences for a game between the two kernels: the fork is the
same K2043 386 Watcom build with patch 0002 (redirector hooks, lifecycle,
FCB/NLS, the INT 24h FS/GS fix) and CIUKIDF.SYS (2,672 bytes); free
conventional memory at the prompt is 7CC4h paragraphs on freedos-1.3, 7C45h
for the fork unbound (2,032 bytes less) and 7BD2h with CIUKIDF resident
(3,872 bytes less); DOS=LOW, FILES, BUFFERS, SHARE and the BLASTER line
are identical. With the IFS, every C: file call is a redirector callout
served natively (2000 ms total budget per request, critical errors through
INT 2Fh/1206h); without it, the fork reads C: through BIOS INT 13h exactly
as freedos-1.3 does. No trap was found in DOOM's startup or play: no budget
expiry, no INT 24h, no PENDING path (the live backend never returns it).

## Watchdog, message and data loss

- The watchdog record keeps `reason=return-or-fault` and now names the
  cause: `cause=hotkey|background|mode-change|present-error|stopping|
  first-frame-timeout|guest-progress-timeout|owner-lost|request`, with
  `stop_reason` and `vector` for `stopping`; `fullscreen=0` names it too.
  Only `stopping` is a VM that stopped by itself (`vector≠0`: a fault).
- A return the kernel decided itself (timeouts, display error) keeps the VM
  running in its window; the DOS Box now shows "Back in the window" with the
  reason ("The game stopped updating the full screen. It is still running in
  this window.") instead of "Full screen unavailable" and `strerror`.
- A VM that stops by itself after it was shown (fault, DOS deadline:
  reasons 2 and 4) keeps the window with "The DOS session stopped" and the
  last console line; EXIT and the owner's own close end it as before.
- Closing the window of a VM whose shell is idle at its prompt is an
  ordinary EXIT, no data loss, as Windows 9x closes an idle MS-DOS Prompt.
  Shell idle: DOS ready, no DPMI client, no V86 INT 21h/4B00h frame
  outstanding (the monitor records each EXEC after DOS ready and retires it
  at the kernel's completed IRET; boot EXECs such as the kernel's own load
  of its shell are not user programs), and the worker in or just back from
  its idle wait. A running program keeps the forced, possible-loss close.
  The armed program's return to the shell is recorded (`launch_returned`).

## Runner case

`f3:vm-game run=<id> [kernel=unified|kernel=freedos-1.3]` (kernel selector,
loader menu and model; not part of sweeps) launches DOOM exactly as
Programs > Games does (desktop `--launch-game=DOOM --game-kernel=<k>`, the DOS
Box's own `--dos-kernel=` and autorun), waits for the prompt, records the VM
text screen (hex rows) at the prompt, before the game's mode set and after it
ends, the unified binding state, three windowed frame digests, then arms
`fullscreen_enter` for the runner's Alt+Enter and samples one record per
second for 45 s (`frames` from the aperture dirty bits, PM timer IRQs,
OPL/SB progress, IFS reads/failures, `cause`). `case=verdict outcome=`
`running` (frames in every observed second but two), `frozen`, `watchdog`,
`exited`, `stopped`, `not-entered`, `no-video` or `launch-failed`.
`fullscreen_demo_screen` keeps the two live QMP screendumps. Cases
`vm-game-doom-unified-qemu-t23` and `vm-game-doom-freedos-qemu-t23` in
`tests/suites/f3-vm.json`.

f3-61b (2026-10-22 10:45, after QEMU run cade4804 on image 6b586080 failed
with "version, run or probe mismatch"): a run carries exactly one probe name
(`scripts/test/evidence.py` refuses any other), and there is no secondary-probe
declaration in the runner. As for `vm-fullscreen`, the selected controller
owns the window path's records from the VM admission on: `window_probe()` in
`dosvm_client.c` names `vm-game` for an `f3:vm-game ` request (admission,
`vm_stop`, `vm_cleanup`, guest and perf records); other runs keep
`vm-window`. Every vm-game record also fits the 240-byte CIUKI_TEST line
for all values (an overlong line becomes `event=ERROR reason=record_overflow`
in `output.c`): text rows are emitted as two halves (`row= col=0|40 hex=`),
the per-second record is `case=sample t= fs= state= mode= frames=n/m dpmi=
returned= cause= error=` plus `case=progress t= irq_pm= opl= sb= ifs_reads=
ifs_failed= ifs_critical=`, the binding state is `case=unified when= kernel=
bound= active= owned= start= critical= refusals= held=` plus `case=unified_io
when= requests= opens= closes= reads= read_kib= failed= expired=`, frames are
`case=frame index= error= mode= w= h= digest=` and the end state
`case=after state= reason= vector= mode= dpmi= returned= exec= idle= cause=`.
Host evidence: `tests/host/test_vm_game_records.py` (widest-value rendering
of every format, the runner Parser and both suite cases over a full run, the
cade4804 prefix refused) and the window-ownership check in
`tests/host/dosvm_video_test.inc`.

f3-61c (2026-10-22 11:08, after QEMU run 12142460 of
`vm-game-doom-unified-qemu-t23` ended "terminal evidence before declared
stimuli completed"): the unified binding and DOOM ran (`DFV2 IFS active
drives=c`, `case=unified when=prompt bound=1 active=1 owned=c start=63`,
1216 IFS reads, 2658 KiB), but the runner's Alt+Enter entry was refused by
the f3-26 readback under QEMU's standard VGA (`fullscreen_transport_fault
registers=1` = MISC output/enable, `fullscreen_refused error=-5
continuation=windowed`, the open f3-29 deviation), so `ARM fullscreen_leave`
never came. Decisions:

- The kernel counts refusals with the windowed continuation
  (`fullscreen_refusals`, `fullscreen_refusal`). The probe waits for entry or
  a refusal and records `case=fullscreen entered= refused= reason=
  continuation=windowed|none policy=qemu|hardware`. Under QEMU
  (`CBI_F_SMBIOS_QEMU`, as f3-29 proposes for this deviation) a refusal is
  data: the probe verifies the windowed game for 25 s (at least 20 sampled
  seconds with a page flip or a changed presenter frame, same `case=sample`
  and `case=progress` records with `fs=0`) and the verdict carries
  `display=window|fullscreen`. On hardware (`policy=hardware`, the T23
  expectation, also when the same case is imported from a physical boot) a
  refusal stays `outcome=refused`, a failure. A profile whose full screen
  works can require it with the predicate field `display: fullscreen`.
- Runner: an action may declare `unless`, a guest DATA record that releases
  it when its ARM never comes (`scripts/test/run.py` `Actions.skip_declined`,
  recorded as `type=skipped`). The vm-game leave action is released by
  `case=fullscreen entered=0`; a missing ARM without that record still fails
  the stimuli.
- Clean end: after the verdict the probe leaves DOOM through its own menu
  (Esc, Up, Enter, y), waits up to 120 s for `dosvm_shell_idle`, then closes
  the window: an ordinary EXIT, `vm_cleanup data_loss=0`. It records
  `case=close method=idle-prompt|forced why=none|vm-stopped|fullscreen|
  game-running|dpmi-client|shell-busy`, then `case=after` and the text
  screen (ENDOOM and the prompt).
- The IFS keeps the first 8 failed requests (function, DOS error, 8.3 name:
  FN1 for path calls, the SFT name for handle calls) and the probe records
  `case=unified_failed when= n= of= fn=11xx dos= error= name=`. In the
  production VM on the host, DOOM's 4 failures are expected misses:
  `1116 error=2 DOOM.ETX` (open), `110f error=2 DOOM2F.WAD`, `DOOM2.WAD`,
  `DOOM.WAD` (get attributes, DOOM's IdentifyVersion).

f3-61d (2026-10-22 11:35, QEMU runs ebffb1db (unified) and 761df914
(freedos case)):

- The freedos case had launched `kernel=unified`: `scripts/test/run.py`
  rebuilds the fw_cfg request from the parsed selector and dropped the
  vm-game `kernel=` suffix. It is now kept before the platform suffix; a host
  test sends both suite cases through `qemu_args` and the kernel selector
  (`PROBE_DOS_FREEDOS` for the freedos case). The probe's `case=vm
  kernel= expected=` predicates already refuse a mismatch.
- The runner failed the windowed run on its own demo pair: the probe's
  per-second presenter hash (a 640x480 render plus SHA-256 under TCG, about
  0.3–0.6 s each) made later samples late, and the two demo ARMs were then
  emitted in the same instant, so the two screendumps were identical. The
  probe now samples cheaply (page flips, scanout seconds) and paces the ARMs
  5 s apart on the clock. With `continuation=windowed` the runner reports
  `fullscreen_demo={skipped, reason: refused}` and compares a desktop
  screendump with later ones every 5 s, from the demo ARM until the verdict,
  for a change that fits one 640x480 DOS window client (`window_demo`).
- Window frame rate under QEMU: not the presenter (the counted flips are CRTC
  start-address writes in the VM, before any WAIT_PRESENT/PRESENT) and not
  the IFS (the freedos-run had the same `frames=1/10`). It is the QEMU return
  tracker (f3-35: TF stepping under `CBI_F_SMBIOS_QEMU` because TCG's debug
  breakpoints are unusable): in the production VM on the host, 60 s of DOOM
  with DMX sound take 5.66 M monitor traps with the TF tracker (4.48 M
  `pm_return` single steps, about 405 per protected-mode IRQ, 11054 IRQs)
  against 1.65 M with the hardware breakpoint tracker (443 k checkpoints,
  397 k `pm_if`), same 138 page flips in that instruction-timed run. The QEMU
  run shows about 5000 VM exits per second (25 k per 5 s perf window), so the
  IRQ handlers' single steps saturate the VM and DOOM's main loop flips only
  every few seconds (3 changes in 19 s). On the T23 (breakpoint tracker, a
  few µs per exit) the same IRQ load costs about 5 % of the CPU; DOOM runs at
  its 35 Hz tic rate, full screen expected near 35 frames per second.
- Profile-aware verdict: full screen, and a window on hardware, need frames
  in all observed seconds but two (`threshold=frames`); a window under QEMU
  needs at least 20 sampled seconds, PM IRQs in all but two of them and at
  least two page flips (`threshold=qemu-tcg`). `case=progress` carries
  `flips=`.

## Open (lead)

- QEMU `vm-game` on both kernels, then the T23 with serial capture: the
  first real IFS activation on hardware (performance and 2000 ms budget on
  the T23 disk are unmeasured; host timing is not hardware evidence).
- The f3-48 QEMU differential (H57-06) ran with the fork unbound before this
  fix; it must be rerun to qualify the live IFS.
