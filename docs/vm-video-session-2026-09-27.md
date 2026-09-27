# Virtual VGA for original DOS programs — 27 September 2026

This record covers the video track built on the
[V86 session foundation](vm-session-foundation-2026-09-26.md) and the
[DPMI contract](vm-dpmi-contract-2026-09-26.md). Original, unmodified DOS
programs running in the experimental `CVSESSION.DLL` session now drive a
virtual VGA through their actual memory and port cycles. The resulting image is
presented into a protected framebuffer, either full screen or inside a native
desktop DOS window.

All results come from QEMU (`-cpu pentium3 -m 128`, KVM, `-vga std`). They do
**not** show compatibility with physical T23/E500 machines or any
hardware acceleration. With KVM, timings follow the host CPU (TSC about
4.2 GHz), not a real Pentium III.

Shared-file changes and the ops the other tracks must accept are listed in
[vm-video-coordination-2026-09-27.md](vm-video-coordination-2026-09-27.md).

The [integration audit](vm-integration-audit-2026-09-27.md) binds these results
to their module, shell and runtime hashes. The combined lifecycle workload is
the unmodified packaged doom-vanille DOS engine `DOOMVAN/PCDMCORE.EXE`
(`efbe64359fb1dfe569cde2428f41ef15a40b8975b8eb36a1cfc5a2731b894980`),
not proprietary original Doom and not the cooperative CiukiOS window port.
Its actual protected CPU memory/port execution is observed; it does not qualify
that game in the desktop window. The initial combined PASS predated the
scheduler reentry and guest-IF corrections; the final combined runs with those
corrections are recorded in the audit's completion section.

## How guest video cycles reach the model

1. **Guard mappings.** The session maps all 32 pages at A0000–BFFFF as
   present, writable and *supervisor* (PTE flags `3`). Every ring-3 access,
   from V86 or from an HDPMI client (VCPI copies the flags), faults. The
   session never maps plain RAM in place of planar VGA memory.
2. **Trap and execute.** The #PF hook decodes the faulting instruction and
   executes it with `cvx_execute` (`src/vm/vga_x86.c`). Each VGA byte is a bus
   cycle on `cvga_state`, in the order the CPU would issue it. The emulator
   handles 16- and 32-bit code, prefixes and segment overrides. It supports
   MOV/MOVZX/MOVSX, ALU and group 1/3 operations, shifts and rotates,
   INC/DEC, TEST, XCHG, XLAT, IMUL, SETcc, moffs forms, segment-register loads
   and string instructions with REP. REP runs at most 4,096 elements per fault
   and then restarts at the guest's next access. An instruction it does not
   implement records a sticky fatal status (`VM_VS_FATAL*`) and is never
   guessed. The owner must then END the session.
3. **Device model.** `src/vm/virtual_vga.c` implements four 64 KiB planes,
   latches, read modes 0/1, write modes 0–3, set/reset, rotate and logical
   operations, bit and map masks, chain-4, odd/even addressing, the memory map
   select, the CRTC/sequencer/GC/attribute registers, the DAC with its
   read/write index state machines and the PEL mask. Scanout covers text,
   mode 13h, unchained 256-colour (Mode X) and 16-colour planar modes. Plane
   writes set dirty bits in 8-byte granules. Changes that alter the displayed
   image increment `display_changes`.
4. **Ports.** Scalar and string VGA I/O (3B0h–3DFh, including `REP OUTSB` DAC
   loads) goes to the same model. Input status 1 follows the model's CRTC
   timing against the TSC, so retrace polling loops make progress.
5. **Virtual video BIOS.** INT 10h in the session runs a model BIOS
   (`src/vm/virtual_vga_bios.c`). It covers mode sets 00h–03h, 0Dh, 0Eh and
   10h–13h, with register tables checked byte for byte against QEMU's
   SeaVGABIOS ROM. It also handles cursor, TTY, scroll, pages, palette/DAC,
   font and state queries, and updates the BIOS data area and INT 43h. The
   physical video ROM is never entered while the session is running. The
   original BDA video fields and the INT 1Fh/43h vectors are snapshotted at
   BEGIN and restored at END.
6. **Protected clients.** The scheduler track's HDPMI 3.24 adapter runs the
   real ring-3 faulting instruction with the same `cvx_execute` and model.
   For a DPMI owner, `src/vm/dpmi_video_fault.c` installs a 0203h #PF handler
   and bridges each access to operation `2Ch` ACCESS through DPMI 0301h.

## Presentation

`src/vm/vga_presenter.c` converts the model's scanout to 15/16/24/32 bpp with
nearest-neighbour scaling, clip rectangles and row damage. It works to a
per-call pixel budget and resumes where it stopped, at the first destination
row of the scaled source row, so one call never runs unbounded. It writes the
uncached LFB with aligned dword stores. A clip change forces a full redraw.
Blink or cursor phase changes redraw only the rows that show a blinking
character or the cursor.

There are two consumers:

- **Timer presenter (full screen).** An INT 08h *observer* hook (it never
  consumes the tick) presents at most once per configured interval and within
  the pixel budget. `VGAHOST.COM` uses it on the deepest 800×600 LFB mode.
- **Native desktop window.** `DOSWIN.DRV` has a VGA session mode that is used
  only when it detects V86 plus the loaded JLM. The native compositor keeps
  drawing through its existing banked path. While the compositor paints, the
  monitor switches to *host mode*: it emulates the compositor's A000 bank
  writes into the protected LFB and emulates VBE 4F05h/4F07h. The compositor's
  real-mode CR0 transition is therefore never called from V86. The window's
  client area is drawn with op `25h` BAND. Op `2Eh` DAMAGE returns a 25-band
  dirty mask (band height = client height / 25, rounded up), so only changed
  bands are redrawn. Host writes are combined into
  64-byte aligned dword bursts before they reach the uncached LFB.

`SHELL.COM` accepts the DOS-window host only in a consistent state. With the
real-mode shell (PE=0), an LFB mode is required. Under V86 (PE=1), the runtime
must report host mode and the banked path must be available. Outside a
session, the normal fullscreen and DOS-window paths are unchanged.

## Resizable DOS window

In VGA session mode the DOS window has a resize grip at its bottom-right
corner (the same grip as the Application Library, now the shared
`ui_grip`). DOSWIN sets header byte `DW_RESIZABLE` (232) while its presenter
can scale into any client. The shell draws the grip and accepts action 63
only then. Dragging keeps the frame between 340×260 (a 320×200 client) and the
desktop work area. The frame is `ui_window_width/height[11]`; the client is
20×60 smaller. `dw_draw` writes it to `DW_CLIENT_WIDTH/HEIGHT` on every paint.
The BAND request scales the model into that rectangle, host damage uses
the same 25 bands, and the virtual INT 33h maps the pointer to the guest
range over the current client. When the guest ends, the window returns to
660×460, because text-mode DOS windows (80×25 cells of 8×16) stay fixed.

The window harness checks this with FIRE running. It shrinks the window to
500×360 and grows it to 760×496. Each time the new client contains only
FIRE's palette, its vertical brightness profile matches the 640×400 one
resampled (correlation 0.993/0.997), the lower half is lit across the whole
width, and the vacated desktop contains no fire pixels. The window is then
restored. In GUESTIO, the centre of a 480×300 client is guest (320,100) and
its corner is (632,196).

SHELL.COM now fills its 0xEF00 arena exactly (60,928 bytes including the
stack). To make room, the DOS window's command tail reuses the shell's
`exec_tail` buffer: DOS copies the tail into the child PSP at EXEC, and
nothing between building it and EXEC uses `exec_tail`.

## Verified on QEMU (Pentium III, 128 MiB)

Reports are in [validation/2026-09-27-vga](validation/2026-09-27-vga/README.md).

| Check | Result |
| --- | --- |
| Model, BIOS and presenter (`scripts/test_virtual_vga.py`) | 168,060 assertions under ASan/UBSan; freestanding OpenWatcom link; mode tables match the SeaVGABIOS ROM records |
| x86 emulator differential (`scripts/test_vga_x86.py`) | 20,000 16-bit and 20,000 32-bit random instructions, identical to Unicorn in registers, flags and memory; divide faults and REP restart points checked |
| Semantics probe `VGASEM.COM` | Runs natively on QEMU's VGA and as a session guest. 27 result words agree except the PEL mask readback, where QEMU returns `00h` (a documented QEMU deviation, accepted only in that exact form). All 5 displayed checkpoints (chained 13h, unchained 256-colour, mode 12h write modes 0–3/read mode 1, mode 0Dh, text odd/even) differ by 0 pixels from native |
| Original mode 13h: DOS Navigator `FIRE.SS` | Unchanged bytes, launched as `FIRE.EXE` because CiukiOS EXEC accepts only COM/EXE names. Palette exactly matches the program's formula; 70 colours presented |
| Original planar/text: Costa `MINES.EXE` | 0 mismatched pixels at every checkpoint, captured only after the display holds still. Two areas are reported separately: the program's own clock digits, and the one cell under Costa's keyboard cursor after the flag keys. Whether that cell's white focus highlight is shown afterwards changes from run to run on native VGA as well as in the session, and stays stable for 4 s either way (`failure-costa-focus-nondeterminism`) |
| Full-screen cleanup | All 32 PTEs, BDA video fields and INT 1Fh/43h restored exactly; physical A000/B800 bytes unchanged during and after the session; display identical after END (only the hardware cursor blinked during it) |
| Native DOS window | VGASEM's five checkpoints in the 640×400 client match native with 0 mismatched pixels. Focus: keys go to the guest only while focused (3 keys withheld, then delivered). Redraw: fully covered by another window, then exposed, 0 mismatches. Minimize/restore: 0 mismatches. `FIRE.EXE` animates in the window with its exact palette and exits when the window is closed. Vectors are restored and memory is released afterwards |
| Monitor and unload | JLM and Jemm unload, and the desktop returns |
| Earlier session gates | V86 legacy gate and DPMI video gate pass with the video-track module `5e584d…`; this is not qualification of later scheduler/IF revisions |

### Measured frame times

| Workload | Measurement |
| --- | --- |
| FIRE, full-screen presenter, 320×200 ×2 → 800×600 LFB | 18.3 presents/s, mean 6.71 ms, max 6.85 ms per 64,000-pixel present; 6.8 full refresh passes/s. 0 "completed" frames/s, because the fire damages every row continuously |
| FIRE, guest execution | 65,000 trapped instructions/s, 10.5 M string elements/s; about 82 % of CPU time spent emulating |
| FIRE inside the desktop window | 18.2 compositor paints/s, mean 29.9 ms per paint |
| Static window repaints (focus/cover/restore) | Mean 69.8 ms, max 97.9 ms per compositor paint in host mode (was 170 ms before write combining); host callbacks mean 2.5 ms |

Conventional memory: 7EACh paragraphs natively, 7E90h with Jemm and the
module, and 7B2Dh inside the session guest.

## Preserved failing evidence and known limits

- `failure-legacy-gate-before-fixture-update.json`: the old V86 gate stopped
  at `running-guest`. Its fixture expected the old plain-RAM behaviour of A000
  in text mode. With a real memory map (B800 in mode 3), A000 reads back
  FFFFFFFFh. The fixture was corrected; the monitor was not changed to satisfy
  it.
- `failure-dpmi-pm-page-fault-before-bridge.json`: the DPMI probe took an
  unhandled protected-mode #PF on the guarded aperture. This was fixed by the
  0203h handler and the 0301h ACCESS bridge.
- **Wolfenstein 3-D** exits with its own out-of-memory message both natively
  and in the session (about 506 KB of conventional memory). It is recorded as
  an equivalent failure, not as a working workload.
- **QEMU PEL mask.** QEMU returns `00h` when 3C6h is read. The model returns
  the written value.
- **Window resize** exists only in VGA session mode (above). Text-mode
  DOS windows outside a session stay 660×460.
- **Input.** With the session's guest devices (see
  [input and sound devices](vm-input-audio-devices-2026-09-27.md)) the guest
  gets raw keys on virtual IRQ1 and a virtual INT 33h mouse. Without them
  (no IF profile) it falls back to BIOS INT 16h keys and no mouse.
- **Close.** COMMAND.COM's FF43h close handshake is unavailable under the
  virtual BIOS, so closing the window sends Esc: a raw make/break through the
  device model when active, otherwise a BIOS key. Programs that ignore Esc are
  not terminated by force. Keys still in the shared BIOS ring when the session
  ends are discarded. A peeked but unconsumed Esc had closed the desktop's
  next Run dialog (`failure-run-field-stale-bios-key`).
- **Launch scene on the hidden page (fixed).** `dos_host_prepare` suspends
  page flipping without flipping back. `ui_pages_prepare` still presented
  single-page scenes to page 0, so when page 1 was visible the launch scene
  (frame, title, task button) never appeared: only the client was painted
  over the old desktop. This showed up intermittently in FIRE captures,
  including the archived baseline, which lacks the DOS task button. Single-page
  scenes now go to `ui_front_base`, and the window harness asserts the
  title-bar pixel.
- **Desktop speed outside a session.** Under Jemm, each real VBE 4F05h bank
  switch costs about 6 ms, so the banked desktop is slow and fast typing can
  drop Run-field keystrokes. The window harness types slowly and verifies the
  field.
- **One foreground session.** Callbacks and the timer presenter serve one
  foreground session. They are not background VMs.

## Building and running

```sh
bash scripts/build_jemm_monitor.sh --ciukios-device-query --ciukios-vm-scheduler
bash scripts/build_vm_session.sh --output build/tests/session
python3 scripts/test_virtual_vga.py
uv run --with unicorn python3 scripts/test_vga_x86.py --cases 20000
python3 scripts/qemu_test_vga_session.py --image <desktop.img> --kernel <CIUKIDOS.SYS> \
  --jemm <JEMM386.EXE> --jload <JLOAD.EXE> --module build/tests/session/CVSESSION.DLL \
  --output build/tests/vga-session
python3 scripts/qemu_test_vga_window.py ... --shell <SHELL.COM> --listing <shell.lst> \
  --runtime <DOSWIN.DRV> --output build/tests/vga-window
```

The monitor is not loaded by any normal live-CD or HDD startup path. The
kernel is unchanged. `SHELL.COM` grows by 48 bytes to 60,880 and remains under
its 0xEF00 image-plus-stack ceiling.
