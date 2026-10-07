# T23 runtime, desktop and media followup

The returned T23 E820 map reports 636 KiB conventional RAM and 510.375 MiB
usable above 1 MiB; evidence is preserved in the physical acquisition dated
2026-10-07. At investigation start the shell reserved only 320 bytes for its stack, while
its own documented measured usage is about 400 bytes. The stack must have
space beyond that measurement before adding further services. The
[NASM flat-binary section documentation](https://www.nasm.us/doc/nasm09.html#section-9.1.3)
defines `nobits` sections with `vstart`: they reserve runtime addresses
without padding the executable on disk. Reserve a 1,728-byte stack after
the paragraph-aligned payload, keep the code/data ceiling EF00h, and shrink
the PSP to include the complete stack. COMMAND.COM retains only its smaller
payload and stack rather than the GUI shell's larger allocation. SS remains
the shell segment. This keeps stack and
code separate within the 64 KiB COM address space; no extended-memory or
DOS heap ceiling is changed. Verify allocation and stack canaries during
the actual DOS/game and desktop tests.

The 14 saved application-state records are also runtime-only zeroed BSS.
Initialize them explicitly before desktop startup, then retain their bytes
along with the stack. This recovers serialized zero padding for the audio
service's packet/PCM bounds checks without increasing the COM memory arena.
Check both the EF00h payload ceiling and the complete 10000h runtime ceiling.

The initial original Doom run stalled in QEMU in an error-reporting wait after
`I_AllocLow` fails, following `V_Init`. Translating the saved VM memory
through CR3=00436000h reveals the requested 256,000-byte block and an actual
largest free block of 176,112 bytes. The root shell still owns 294,656 bytes
of conventional memory. Reading physical low memory without translating
the guest page tables instead inspects the master VM and gives a misleading
free-memory result. The wait compares a zero global counter against 30,
but the game has not yet installed its timer; this does not justify an IRQ
patch. The physical photograph shows a V86 #GP at EIP=10000h. Keep these
observations distinct.
The [DPMI interrupt-handling specification](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.html)
requires installed protected-mode hardware handlers to receive interrupts
in place of default real-mode reflection, and permits explicit chaining.
Check the repository adapter against the
[pinned HDPMI source](https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/HDPMI.ASM)
before altering IRQ delivery. A changing CVSESSION delivery counter does
not itself prove that the game's timer handler ran.

The media API uses one foreground stream with explicit ownership. Native
Player calls shell service 31, which passes SFX driver operation 8 and a
30-byte packed packet with sample rate, total/played/queued frame counts,
far PCM address, frame count, volume and flags. Operations are open, write,
poll, pause, seek/flush, volume and close. PCM is signed 16-bit stereo at
48 kHz; each write is bounded to 2,048 frames. Flags bit 0 is pause, bit 1
is end of stream. Cues yield to the music owner; DMA start/poll/stop occur
only in foreground callbacks. Codec decoding belongs to a bounded 32-bit
worker using existing owned shared-memory services. Decoder licensing,
format behavior and exact pinned source inputs are recorded with the
codec bundle. Do not cache complete decoded tracks on the 128 MiB image.

Mailbox teardown follows the repository's CVSESSION VM contract in
[`session_abi.inc`](../../src/vm/session_abi.inc) and
[`session_vmm.inc`](../../src/vm/session_vmm.inc): status reports a VM as
free, running, or ended and includes its generation; `VMM_KILL` returns an
error through carry and releases the VM when successful. The worker maps the
locked XMS mailbox for its entire process lifetime. Therefore a successful
kill request alone is not the release condition: `mediawork_close` must
re-query the VM slot and generation, and keep the mailbox locked if the VM is
still present or status failed. A free slot or changed generation proves the
old worker no longer owns the mapping. The prior ended-state path discarded
the mailbox immediately after issuing `VMM_KILL`, even when the call failed.

The production run result and its exact installed VM-file provenance are
recorded at the end of this document.

## Canonical-image regression results

The capped sequential Linux full build completed with all 17 native modules,
the decoder worker, and matching kernel/LFN/CVSESSION/VMFORK build IDs. The
Windows portable ZIP passed an archive CRC check; it has not been run on Windows.

`build/full/t23-vbe-fix/desktop-apps-current/report.json` passes the full desktop
application gate, including copy, move, recycling, nested folder navigation,
drag-to-folder, Notepad persistence, keyboard shortcuts and Tasks window close.
`aperture-current/` passes standard-VGA VM rendering, Cirrus VM banked rendering,
and standard-VGA NOVM linear-framebuffer rendering, including damage below the
64 KiB framebuffer row boundary. These results are emulator evidence only.

The initial canonical music run played WAV, MP3 and Ogg Vorbis through real
AC'97 DMA past one 64 KiB ring, then failed after the third decoder VM ended.
Its failure frame and bounded diagnostics remain preserved in
`music-canonical-diagnostics/`. That historical failure was later superseded by
the corrected production-profile run below; it is not the current music result.

## Earlier diagnostic runtime outcomes

These earlier experiments document failures and profile distinctions. They
predate the final canonical passing run below and do not override it:

- `music-no-r0-active-runtime` used the correctly named `CVSESS.DLL` built
  without the ring-0 switch. It failed after WAV EOF with a guest #GP at
  `FF53:0010`; the WAV worker had ended. This is a real failure of that
  configuration, not an untested diagnostic build.
- `music-no-r0-runtime` placed the DLL under the incorrect filename
  `CVSESSION.DLL`, so the DOS loader did not exercise the intended CVSESS
  replacement. That run played all four formats and passed Previous
  navigation, then faulted during close/reopen of Ogg with #06. It is useful
  playback evidence, but it is not evidence for the no-ring-0-switch change.
- `music-xms-guard-runtime` exercised the XMS-guarded production profile. WAV
  and MP3 reached EOF; the following Ogg attempt faulted before the Player
  logged its open path. The captured frame reports #06 at `3146:1197` with the
  XMS-guard counter still zero. This run failed and does not prove all-format
  playback or identify an XMS callback as the cause.

The older `music-canonical-diagnostics` capture remains distinct: it contains
the WAV/MP3/Ogg sequence and fault after the third decoder VM ended. Its dump
was captured with the recorded Jemm artifact from that run; conclusions about
that capture's profile must not be applied to the newer no-ring-0 or
XMS-guarded runs. These runs need a fresh, correctly named DLL and matched
binary/listing provenance before comparing scheduler variants.

The first Viewer image harness run exposed a BMP cache regression: its failure
frame showed a black image and the status remained `BMP image: decoding`
(`build/full/t23-vbe-fix/viewer-first/failure.png`). The BMP scanline source
aliased the cache merge row, which the merge cleared before copying. BMP now
decodes into a separate RGB scratch buffer. The same run exposed an early
`decoded` flag that stopped `EV_POLL` before multi-row cache finalization; the
flag now means the cache is fully initialized, and in-progress polling returns
the busy-only result. BMP work is bounded to four source rows per poll. This
records the observed failure and source correction. The subsequent passing
reruns are recorded below.

The second Viewer runtime run rendered the 24,000-pixel BMP8 fixture with exact
pixel matches, then failed the 4× zoom step: the harness injected wheel motion
but no zoom change was observed. Shell wheel dispatch resolves its target from
the app hit map, and the Viewer had not registered its image viewport there.
An explicit viewport hit was tried, but hit 7 shadowed the window-body hit 70
and blocked body mouse events. That overlay was removed; the normal body hit
70 now routes wheel and drag input. Keyboard and toolbar `+`/`-` zoom remain
available, and serial logging records only delivered input transitions.

The earlier all-formats Viewer rerun passed in QEMU at 1024×768; its report is
`build/full/t23-vbe-fix/viewer-all-formats-current/report.json`. Files opened
each of six generated fixtures (8-bit, 24-bit and 32-bit BMP, GIF, JPEG and
PNG), and the rendered 24,000-pixel images matched their reference exactly
except for expected JPEG loss (mean channel error 0.67, p99 4). The harness also
verified 100% view, 4× zoom, arrow and mouse pan, Fit restoration, Next/Previous
including `.JPEG`, truncated-BMP rejection, and close/reopen. The 4× visible
image comparison covered 72,960 pixels with exact matches. The fixture source
image stayed unchanged. This was emulator rendering evidence, not a physical
hardware qualification.

The serial-only DOS window messages left no useful launch trail on the
returned physical disk. Keep an event-only, best-effort `SYSTEM\\DOSVM.LOG`
bounded to 4 KiB: record the full requested command, resolved EXEC command,
fork/service error code, VM/generation and program exit. The existing DOS
create/open/seek/write/close wrappers map to the same handle lifecycle used
by the [upstream FreeDOS FAT implementation](https://github.com/FDOS/kernel/blob/master/kernel/fatfs.c).
Logging does not execute in an IRQ or drawing loop; one-time presenter errors
are deferred to the next foreground poll. Media file icons append Tango's
`image-x-generic` and `audio-x-generic` to the stable icon ABI and existing
palette; the approved Ciuki portrait is preserved.

`DPMIRUN.LOG` also persists the launcher's existing foreground serial stage
messages. Writes preserve register/flag state, cap each fragment to 160 bytes
and rotate before 4 KiB. No IRQ callback writes the log. This identifies
whether a physical failure preceded host installation, session setup, EXEC
or teardown; it does not fabricate a saved exception frame.

The first audio runtime attempt reached HDPMI but COMMAND could not find
the worker. `MEDIAWORK.EXE` has a nine-character stem; mtools stores a VFAT
alias, while the classic EXEC path expects the DOS name. The
[Microsoft 8.3 filename specification](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/18e63b13-ba43-4f5f-a5b7-11e871b71f14)
limits the stem to eight characters. Resolve the installed worker through
the repository's existing `dos_short_path` service in the parent desktop,
before allocating/spawning its mailbox, and pass that alias to COMMAND.
This keeps the installed descriptive filename and avoids assuming the
alias assigned by a particular image builder.

## Player close lifecycle observed before the fix

The earlier `music-farret-runtime` recorder run completed all four formats and
Previous navigation without a frozen invalid-opcode snapshot. Its close/reopen Ogg step
left the Player window visible with stale `Playing` state after decoder end.
Source inspection identifies a close-request lifecycle gap: `player.c` returns
1 from `EV_CLOSE` while `mediawork_close()` is pending, which intentionally
keeps the module loaded. Later `EV_POLL` observes worker teardown and clears
`closing`, but never retries closing the specific Player window. The application
API already provides `app_window_cmd(window, 2)` for a named window; the shell
poll dispatcher defers that targeted command safely until after the current
poll iteration. The Player remembers a pending user close
and issues the command for `WIN_PLAYER` once teardown is complete. This avoids
`app_close()`, whose target is the currently active window and could change
during worker teardown. The source contracts are implemented in
[`player.c`](../../src/apps/player.c), [`app.h`](../../src/apps/app.h), and
[`shell_apps.inc`](../../src/com/shell_apps.inc).

## Corrected production profile-release run

`build/full/t23-vbe-fix/music-shadow-runtime2/report.json` passes the real
music workflow against a private copy of the full HDD image. WAV, MP3, Ogg
Vorbis, and FLAC all advanced beyond 16,384 stereo frames and reached end of
track. The run completed six worker sessions and six worker ends, exercised
pause/resume, volume, seek, Previous, close/reopen of Ogg, and Stop, and
confirmed that the Player window and module were unloaded after close. The
audio capture's sustained chirp rose from 330 Hz to 370 Hz over eight windows.
This is QEMU evidence; `physical_hardware_qualified` is false.

The run used VM files verified directly from its copied disk image:
`VM/JEMMEX.EXE` SHA-256
`01ba4421c3f908b965df9151de95123eaf68d3320e48c412d243568ca39a7b31`,
`VM/JEMM386.EXE` SHA-256
`2b38684adf5122242174ebc17bfd6732adfd74c44915498fa749e50dabce477f`, and
`VM/CVSESS.DLL` SHA-256
`932a04f7027cbd3c55670afbb7c246b8c8af510dc7ce30756c1cb68104cac47f`.
The earlier `music-shadow-runtime1` image put the Jemm executables in the
wrong location and did not exercise this profile-release change; its result
does not qualify the fix. The passing run is
`music-shadow-runtime2/serial.log` and its adjacent report and copied image.

## Latest canonical media run

`build/full/t23-vbe-fix/music-properties-final2/report.json` is the current
passing music result. Its source HDD image is
`build/full/ciukios-full.img`, SHA-256
`3c0d1491700375bdb7aaa1072cbbb9d23b06c280e3adbc818408b5626739b2fa`; the
private test disk is SHA-256
`669f7d8e4fe2fb28e36f974593500cfe72d87fa74e7eaac3cfa7b40e3ad43254` after
test fixtures and logs were added. The tested copy and canonical source have
matching hashes for the shell, Player, media worker, Jemm and CVSESS binaries:

- `SYSTEM/SHELL.COM`: `bd9434408b67b668cae49b57ce0c5a922c2c066f600225af09511f161abba056`
- `SYSTEM/APPS/PLAYER.APP`: `5152074518c27c395b1b0e270515b230e3f56914be9b74ddb800d04310aeb1de`
- `SYSTEM/APPS/MEDIAW~1.EXE`: `66b723a8c773462f3a937bfb413ac83a6d8fb10b23a409572488a35a3a347a16`
- `VM/JEMMEX.EXE`: `01ba4421c3f908b965df9151de95123eaf68d3320e48c412d243568ca39a7b31`
- `VM/JEMM386.EXE`: `2b38684adf5122242174ebc17bfd6732adfd74c44915498fa749e50dabce477f`
- `VM/CVSESS.DLL`: `2649e93901cc0996e147d2179057f0e31ab56980b1c861d553abfeccd83013df`

The workflow played WAV, MP3, Ogg Vorbis and FLAC through EOF, with each
format advancing beyond 16,384 stereo frames. It also checked pause/resume,
volume, seek, Previous, close/reopen of Ogg, and Stop. The run observed six
worker sessions, six ends and six idle yields. After the explicit Player
close, `window_flags` and `module_segment` were both zero, confirming the
deferred close lifecycle correction recorded above. The report has
`app_return_trace_enabled: false` and
`physical_hardware_qualified: false`.

The captured WAV contains 5,808,704 bytes of signed 16-bit PCM at 44,100 Hz
(5,808,748 bytes including the WAV header). Of the PCM bytes, 5,151,899 were
nonzero, including 4,867,954 after boot. The captured chirp rose from 350 Hz
to 510 Hz across eight analysis windows. These measurements provide direct
audio-output evidence for this QEMU run.

## Viewer evidence

`build/full/t23-vbe-fix/viewer-final/report.json` belongs to the earlier HDD
snapshot with SHA-256
`f8e17a92afba4a5897efbd57b82bbbb2475031423878cc0bc987db048c9b2cca` and remains
historical evidence. The fresh gate
`build/full/t23-vbe-fix/viewer-properties-verified/report.json` passed against
the current canonical HDD image, SHA-256
`3c0d1491700375bdb7aaa1072cbbb9d23b06c280e3adbc818408b5626739b2fa`. It
verified all six BMP/GIF/JPEG/PNG fixtures, exact-pixel 4× zoom over 72,960
pixels, arrow and mouse-drag pan, Fit restoration, Next/Previous navigation,
JPEG navigation, rejection of a truncated BMP, and close/reopen rendering.
The report records `source_unchanged: true`; this is QEMU evidence and does not
qualify physical hardware.

The newer desktop and Files selection gate
`build/full/t23-vbe-fix/selection-properties/report.json` passed marquee,
Ctrl-toggle and multi-file copy/move/recycle with FAT content checks. Its
retained test copy has the same SHELL, Desktop and Files module bytes as the
latest HDD; only Player changed among the application modules. The earlier
`selection-final` report is historical and remains distinct.

## Rebuild identity and DOOM provenance

The retained `selection-properties/disk.img` comes from the same
`6514765d51bc4d839788f1ec749065204b98ed254cbca2d3b2097188112e5959`
source snapshot as the two-launch `doom-properties-final` gate. Its DOSVM,
Display, DPMIRUN, Jemm, DOOM wrapper/core and TestGames launcher are identical
to the latest HDD. CVSESS.DLL has equal length (242,176 bytes); its only
different bytes are at file offsets 0x80–0x81. The DOS header gives
e_lfanew=0x78, so those bytes lie within the timestamp at header+8. The values
decode to 2026-10-07 17:31:14 UTC and 18:00:16 UTC, the respective build times.
All header bytes outside that field and all section contents match.

The [upstream JWlink PE writer](https://github.com/Baron-von-Riedesel/jwlink/blob/master/c/loadpe.c#L1185-L1203)
selects PX_SIGNATURE for the HX format and assigns time(NULL) to time_stamp
in the same header. The [Microsoft PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
defines the timestamp field at that offset. CiukiOS invokes JWlink's HX DLL
format in build_vm_session.sh. This establishes an unchanged VM code/data
image across this rebuild; it does not turn the emulator DOOM result into
physical T23 qualification. The later final release rebuild must receive the
same section comparison before its HDD is written.
