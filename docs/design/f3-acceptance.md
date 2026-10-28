# F3 DOS VM acceptance (f3-00)

Status: implementation and host validation; QEMU and hardware evidence belongs
to the lead. This document defines the first F3 row, not completion of F3.

## Research and implementation decisions

Checked 2026-10-11 before implementation:

- [Intel IA-32 SDM volume 3, chapter 15](https://kib.kiev.ua/x86docs/Intel/SDMs/245472-004.pdf)
  and [the current Intel manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html):
  V86 has 64 KiB segment limits even with address-size overrides; IOPL=0
  traps the sensitive instructions. Extend the existing `v86.c` monitor,
  keep physical IF set, VME/PVI clear and the TSS deny-all bitmap. Memory
  copies must support noncontiguous NORMAL pages, including page zero.
- [SeaBIOS 1.16.3 disk services](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/disk.c):
  CHS cylinder encoding, EDD DAP fields and returned transfer counts inform
  the independent virtual implementation. Validate the complete guest buffer,
  packet and disk extent before I/O; never forward a guest drive to firmware.
- [SeaBIOS keyboard services](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c)
  and [clock services](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/clock.c):
  status calls preserve queued keys; waits suspend only the requesting VM.
  DOS time derives from the VM timer, and memory answers are synthetic.
- Repository comparison: F1's BIOS VM borrows firmware memory and owns a
  physical input lease. DOS VMs instead allocate private NORMAL pages and
  use synthetic ROM stubs. The existing PIC/PIT monitor state is per instance.
  F2 native backing is eagerly reserved by `uaddr.c`, so physical free counts
  already subtract its commitments, kernel, drivers and framebuffer.

Private disk choice: a read-only VFS base plus a file-backed per-generation
scratch overlay in `C:\SYSTEM\DOS\`. No whole image is copied. The overlay
is ephemeral: normal exit commits it before removal; forced exit records
possible private data loss. Its bounded sector index is charged to the VM's
low-memory budget. The contract's file-backing requirement takes precedence
over the directive's permission to choose RAM or disk.
The index holds at most 1024 changed sectors (512 KiB of private file data),
returning ENOSPC before an additional write. A failed deletion retains the
generation and VFS table for cleanup retry; a slot cannot be reused while
its private file remains. The lifecycle probe explicitly opens the existing
F1 C: write gate, since test boots mount volumes read-only first.
A per-VM sleeping mutex protects the overlay index and each VFS seek/sector
transfer pair, including cleanup. A guest disk service may sleep while the
native controller runs; their shared file position must not interleave.

Shell choice: reuse `src/com/shell.asm` assembled with `COMMAND_COMPAT=1`.
This existing DOS command interpreter shows a prompt, implements DIR/TYPE,
EXEC by name and EXIT without the desktop startup path. A small DOS bootstrap
runs AUTOEXEC.BAT and then COMMAND.COM, reporting the shell's return through
the VM-local control port. The 0.8 FAT16 boot sector, thin loader and
`src/runtime/ciukidos.asm` (which includes `floppy_stage1.asm`) build only the
VM payload, using the legacy full-profile geometry and recipe. No floppy,
FreeDOS, Jemm or HDPMI build is involved.

## Selector and records

Ordinary selector: `f3:vm-boot run=<8 hexadecimal digits>`. No optional suffix.
F3 also joins the cfg-only sweep tables. Existing ordinary F0–F2 selectors
retain their meaning; the all-phase sweep includes the F3 phase.

Record envelope remains `CIUKI_TEST v=1 run=... seq=... probe=vm-boot event=...`.
Guest byte port `0xE9` accepts ASCII lines of at most 80 bytes, at most 16
lines per 1000 host ticks. CR is ignored, LF frames a line. Overflow/non-ASCII
discards through LF; excess rate drops complete lines. DATA group=guest records carry
VM id, generation and hex-encoded bytes; guest text cannot supply an envelope
or a trusted PASS. DOSREADY reports measured DOS version, failed-largest-
allocation BX (free conventional paragraphs) and INT21/19 drive index.
Each accepted line is split into at most two DATA group=guest records with `part` and
`parts`, at most 48 guest bytes per record, keeping the full envelope within
240 bytes. Invalid/rate-limited lines increment the VM drop counter.
Control port `0xEA`, byte OUT value 0, requests clean shell exit; other values
are faults. Unregistered ports read all ones at the requested width and ignore
writes, except the documented forbidden probe port `0xFFFF`, which faults.
Physical passthrough is never an option.

INT1A/00 and /01 expose the private BIOS tick count; the default virtual
IRQ0 stub updates it and invokes the guest INT1C hook. INT1A/02 and /04 use
per-VM elapsed scheduler ticks from the synthetic epoch 2026-10-11 00:00:00
UTC. No CMOS/RTC or host BIOS call is performed. The epoch is synthetic,
not an observation of the physical machine's wall clock. INT15/86 waits
and INT16 blocking reads sleep only this worker; the core also enforces a
30-second deadline before DOSREADY, including a halted VM.
Both blocking and status keyboard calls pump a bounded number of existing
INPUT_TEXT events for the latest booted VM. Events predating that boot are
discarded; explicit VM key/IRQ producers validate the supplied generation.
Complete scan-code/device routing remains f3-01/f3-02.

## Delivered row and lead gate

`vm-boot` must reach DOSREADY and the text prompt, then complete 100 clean
create/boot/EXIT cycles and 100 create/fault cycles alternating a privileged
instruction and forbidden-port payload. A concurrent witness VM retains its
IVT and disk sentinels, and a native heartbeat must make progress. DATA rows
report boot, cycles, isolation, admission arithmetic and cleanup baselines;
END PASS requires every check. Admission refusal is explicit and never counted
as a successful lifecycle iteration. Both VMs use an 8 MiB runtime commitment
for this low-memory probe; the normal maximum remains 32 MiB, with 32 MiB host
headroom. Reservation and resident bytes are reported separately.

The lead runs `f3-vm` on qemu-t23, qemu-e500 and qemu-min128, after the F0–F2
prerequisites, plus `f2-all` on the same image SHA-256. Host tests establish
model and failure-path evidence; they do not establish a DOS boot or QEMU PASS.

## Host verification

The deterministic FAT16 writer was checked against Microsoft's
[FAT specification, sections 3.5, 4 and 6](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf):
the legacy geometry yields 32722 data clusters, within the FAT16 range;
directory records, fixed DOS dates and both FAT copies are emitted explicitly.
Independent fsck.fat and mtools readback verify the resulting payload.

`tests/host/dosvm_test.c` links the production lifecycle, BIOS, disk and V86
code and extracts the unchanged production paging functions. PMM, CPU,
scheduler and VFS transport are the host boundaries. It injects failure at
277 allocation sites and four VFS creation sites and checks exact teardown,
including A20 alias restoration. Other cases exercise discontiguous memory,
ROM protection, CHS/EDD preflight, overlay isolation/capacity/errors, stale
generations, private reset, blocking keyboard reads and record framing/rate.
The existing V86 firmware tests add discontiguous stack/string copies,
operand/address-size overrides, restart after a segment fault, synthetic
INT interception and invalid IRET destinations. Suite parser fixtures reject
99-cycle counts, missing prompt/heartbeat, resource deltas and failed admission.
The suite requires 202 successful admissions and cleanups (witness, warm-up
and 200 measured VMs); guest lines cannot satisfy these controller predicates.
Controller records use the existing DATA event with `group=admission`,
`group=vm_stop` and `group=vm_cleanup`; guest bytes use `group=guest`.
This keeps the common parser's event grammar unchanged.

Verified 2026-10-11: the full `make test-host` target passed (222 Python
tests, five skips, C/loader checks). The final DOS VM harness, including
keyboard status pumping and stale input rejection, passed 99485 checks
with ASan/UBSan; its largest complete record was 212 bytes. The extended
V86/firmware harness passed 140585 checks. Kernel FPU/SIMD audit and SDK,
Lua and desktop builds passed. T1 checked 56 canonical-image payloads,
including the 128 MiB DOS payload and its ten files.

Built-image SHA-256:
`0a5d309ab56569fb5530fdb805188b4be9f1276acd029362df216697a8dc0da4`.
DOSBOOT.IMG SHA-256:
`13714a2dce32b7456c20c4d1e9c69670367ce7e4021430f79194f87f77518251`.
These hashes identify static build evidence only; the lead's QEMU acceptance
is still pending.

## f3-01

Research checked 2026-10-11 before implementation:

- [Intel 8259A datasheet](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8259A.pdf)
  and [QEMU 9.2 PIC](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/intc/i8259.c):
  retain the monitor's authoritative PIC pair, move its register/arbitration
  helpers into `dosvm_pic.c`, and latch the cascade request. An empty slave
  at acknowledge returns slave-base+7, sets master ISR2 only, and requires
  only master EOI. Empty master acknowledge returns master-base+7 without ISR7.
  ICW3 and ICW4, OCW polling, special mask, rotation and auto EOI are private.
- [Intel 8254 datasheet](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf):
  migrate the legacy latch/access model, extend modes 1/4/5 and BCD counting,
  preserve independent count/status latches, and select read-back channels
  with set bits 1–3. Channel 2 gate is port 61 bit 0; bit 5 reads OUT2.
  Host time is accounted first, at 1193 PIT clocks per scheduler tick.
  Catch-up is capped at 64 host ticks per call; excess is counted and dropped
  from device advancement while the VM clock advances by the full elapsed
  interval. Arithmetic advancement bounds work independently of the divisor;
  multiple edges coalesce in the PIC IRR, with expiry/drop counters exposed.
- [QEMU PS/2 model](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/input/ps2.c)
  and [SeaBIOS keyboard BIOS](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/kbd.c):
  normalized desktop KEY codes already use set 1. Queue whole encoded
  sequences, keep controller replies separate from key state, and generate
  typematic only for host repeat events. The synthetic INT09 consumes bytes,
  updates the private BIOS keyboard queue, and EOIs the virtual PIC. INT16
  remains in f3-00. Focus loss discards stale queued makes and queues breaks
  for every held key; desktop TEXT must not duplicate KEY-derived input.
- [CuteMouse upstream INT33 interface](https://github.com/FDOS/mouse/blob/master/int33.lst):
  implement polling and a validated V86 far-call continuation, never a host
  function pointer. The callback returns with RETF to a private ROM INT stub;
  the monitor restores its saved frame and virtual IF. Callbacks are serialized,
  deferred while IF is clear or a shadow is active, and revoked on stop.
  Default text coordinates span 640×200 in 8×8 units. Text cursor decoration
  is applied to the exported cell copy, preserving guest video memory.

The desktop integration boundary is kernel-internal and generation checked:
`dosvm_input_focus` and `dosvm_input_event`. f3-02 owns its authorized native
transport and DOS window; the existing desktop v1/v2 wire format is unchanged.
The existing shell-only input poll remains available until an explicit window
claims focus. No physical controller, PIC, PIT or PS/2 access is added.

Host/build and lead evidence are recorded below after implementation.

### Lead findings while qualifying f3-01 on QEMU (2026-10-11)

Codex stopped at its usage limit; the lead merged `main` (f3-03 services and
the DOS VM boot fixes) into the row, gave the i8042 model sole ownership of
ports 60h/64h (the f3-03 inline controller state was removed), named the
VM records by the owning probe (`dosvm_config.probe = "vm-devices"`) and
made the payload path absolute (`\VMDEV.COM`). The first QEMU runs reached
the `cli` and `timer` stages; the `shadow` stage failed with one extra tick
and the timer handler's interrupted IP at the `out 21h` instruction: the
payload loaded AL *between* `mov ss` and `out`, so the MOV SS shadow ended at
`mov al` and the pending IRQ0 was recognised before the OUT, as a real CPU
does. The payload now loads AL before STI, so the STI shadow covers MOV SS and
the MOV SS shadow covers the trapped OUT. The timer handler records the
interrupted IP (`last_irq_ip` in `case=shadow`). The monitor also keeps a
MOV SS/POP SS shadow across a single-step trap raised at the successor
(`shadow_next`), a robustness measure for an emulator that does not suppress
the #DB after MOV SS; real CPUs never reach that branch. A kernel-side record
per delivered IRQ was tried and removed: its serial cost broke the timer
stage.

### Keyboard stage diagnosis (2026-10-11)

The QEMU timeout record at stage 5 (`ticks=10054 keys=0 irr0=03 isr0=00
imr0=fe queued=2`) is correct 8259A behaviour for what the payload wrote.
[Intel 8259A, p. 13](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8259A.pdf):
"M = 1 indicates the channel is masked (inhibited), M = 0 indicates the
channel is enabled"; bit 0 masks IR0. `VMDEV.COM` wrote OCW1 `FEh` for
"IRQ1 only", which enables IR0 and masks IR1, so the 1 kHz timer ran and
the two queued scancodes waited behind a masked IRQ1. The payload now writes
`FDh`. `test_device_irq_delivery` (host) replays the payload's ICW/OCW and
PIT programming, the host key events, STI with its boundary trap and HLT
through `dosvm_trap`, `v86_emulate`, `v86_irq_deliver` and the ROM stubs:
`FEh` delivers vector 8 with IRQ1 still queued (the QEMU record), and `FDh`
delivers vector 9 twice (make, then host typematic) and never vector 8 across
about 50 masked PIT edges during HLT.

The same replay of stages 6 and 7 found two more defects, both fixed:

- **Mouse continuation overwritten.** f3-03 put the EMS INT 67h entry
  (`INT FE; IRET`) and its `EMMXXXX0` name at F000:1000–1011, the address
  the INT 33h callback continuation (`INT FD; HLT`) already used, and
  `dosvm_memory_init` runs after `dosvm_bios_init`. Every callback's RETF
  therefore entered the EMS service and then an IRET over a non-interrupt
  stack. The continuation now has its own slot at F000:1020
  (`DOSVM_MOUSE_RETURN`); a host check reads both ROM layouts back.
- **INT 33h replaced by CiukiDOS.** `init_stage2_services` installed the
  built-in driver, whose reset fails without PS/2 hardware
  (`ENABLE_PS2_MOUSE_INIT=0`), and `init_mouse` then pointed INT 33h at an
  IRET stub. Every DOS program in the VM, `VMDEV.COM` included, lost the
  VMM's mouse. The [CuteMouse INT 33h list](https://github.com/FDOS/mouse/blob/master/int33.lst)
  defines detection as AX=0 returning FFFFh after checking that the vector
  is neither 0000:0000 nor an IRET. The DOS VM personality now sets
  `CIUKIDOS_FIRMWARE_MOUSE=1` (`scripts/build_dos_image.py`): CiukiDOS
  keeps the firmware INT 33h and INT 74h vectors, records no built-in
  driver and lets INT 15h/C2 chain. The default (0) assembles byte-identical
  to the previous kernel.

The payload also clears the whole 28-byte mailbox, including `last_ip`.
`tests/host/record_scope_test.py` lists `probes/f3_probes_devices.c` as an
evidence producer, like the other probe files.

Open items for the lead: the EMS name is found at handler offset+0Ah
(CiukiDOS and `VMMEMORY.COM` check it there), not at segment:000Ah where the
"get interrupt vector" method of
[LIM EMS 4.0](https://lo-tech.co.uk/wiki/LIM_Expanded_Memory_Specification_V4)
looks, because the vector is F000:1000. Separately, port 60h returns FFh when the output buffer is empty,
while QEMU's `ps2_read_data` returns the last byte ("needed for EMM386").

### Wait loops (QEMU, image from a9e3845, 2026-10-11)

With `FDh`, both key events arrived through vector 9 (`keys=2 isr1=2`,
`irq1=2`, ISR clear) but stage 6 never came. The stopped frame had `AX=00FDh`
and `SP=FFFAh` with `0000h,1746h`, which is CiukiDOS's COM entry frame
(PSP:0000 pushed below SP=FFFEh), so the guest was halted in its main loop.
`.wait_keys` was `HLT; CMP keys,2; JB`. The two IRQ1s became deliverable at
STI and were taken at the boundary after `mov word [stage],5`, because the
[STI](https://www.felixcloutier.com/x86/sti) inhibition "ends after ... the
execution of the next instruction". They returned to the HLT, which then
waited for an interrupt that never came (IRQ0 masked, keyboard drained). A
real CPU behaves the same; the monitor's delivery at that `#DB` boundary is
correct. `.wait_keys` and `.wait_focus` now test their condition with IF
clear and sleep with `STI; HLT`. The shadow then covers HLT, so an interrupt
pending at STI is taken after HLT and wakes it. The host replay reproduces the hang
(old loop: still halted after 200 ms, AX=FDh, IP after HLT) and runs the
corrected loops, including a focus release made pending before `STI; HLT`.

## f3-02

Status: implementation and host validation complete (2026-10-11, Claude as
implementer after Codex's partial commit 1c33fb8); QEMU, desktop screenshot
and hardware evidence belong to the lead. This section makes no runtime PASS
claim.

### Research checked before implementation

- [QEMU 9.2 `hw/display/vga.c`](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/display/vga.c):
  `vga_mem_readb/writeb` show that mapped RAM alone cannot implement latches,
  chain-4 or planar write modes; input status 1 is computed per read
  (`vga_precise_retrace` sets retrace plus display-disabled in vertical
  retrace and display-disabled alone in horizontal blanking; the dumb method
  toggles both per read) and every read clears the attribute flip-flop.
- [FreeVGA, Input Status #1 Register](https://web.stanford.edu/class/cs140/projects/pintos/specs/freevga/vga/extreg.htm):
  bit 3 vertical retrace; bit 0 set during horizontal or vertical retrace.
- [VESA VBE 3.0](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf):
  AL=4Fh function recognised, AH=01h call failed (contract's unavailable-VBE
  convention through F4).
- [Unicode CP437 mapping](https://www.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/CP437.TXT):
  80h-A5h accented letters, B0h-DFh shades, box drawing and blocks, F9h, FAh,
  FEh, FFh.
- [HelpPC keyboard scan codes](https://stanislavs.org/helppc/scan_codes.html):
  INT 16h scan/ASCII words, including shifted/Ctrl/Alt function keys, Ctrl
  navigation keys (73h-93h), Alt digits (78h-83h) and Ctrl punctuation.
- [Abrash, Graphics Programming Black Book, ch. 47](https://www.phatcode.net/res/224/files/html/ch47/47-02.html):
  the Mode X 320x240 sequence (SR04 06h, CRTC 14h/17h byte mode, misc E3h,
  CRTC 06h/07h/09h/10h-12h/15h/16h) used by `VMVIDEO.COM` stage 4.
- The retained models (`virtual_vga.c`, `virtual_vga_bios.c`, `vga_x86.c`,
  `vga_presenter.c`, migrated unchanged apart from V86 64 KiB segment-limit
  checks in `vga_x86.c`) carry their own register/BIOS references
  (Chips & Technologies 82C452, DOSBox-X chained-memory notes, SeaVGABIOS
  tables). They implement the semantics independently; the old Jemm/HDPMI
  adapters are not used.

### Decisions

Aperture: every `A0000-BFFFF` guest PTE stays supervisor-only. A guest #PF
there (classified by the CR2 captured before interrupts are re-enabled) runs
the faulting instruction through `cvx_execute` on a per-VM bus: aperture
reads load the latches, writes use the GC write modes, other operands use
the checked private-memory helpers, instruction fetch from the aperture
fails. REP strings run 4096 elements per fault and restart like an
interrupted REP; a divide error reflects #DE through the guest's INT 0
vector; unsupported forms, 64 KiB segment-limit crossings and bus errors
stop only that VM. Kernel-side `dosvm_read` of the aperture peeks
(no latch load); kernel writes go through the model. A direct plane or
shadow mapping for fast mode 13h/Mode X workloads is left to f3-06.

INT 10h: the f3-00 ROM stub reaches `cvbios_int10` (modes 00h-03h, 0Dh,
0Eh, 10h-13h, cursor, scroll, teletype, palette/DAC, fonts, 12h/1Ah). Guest
buffers (1002h/1009h, 1012h/1017h, 1100h/1110h, 13xxh) are validated whole,
direction included, before any register or VRAM change. 4Fxxh returns
AX=014Fh. Text mode sets select eight-dot character clocks (SR01 bit 0), so
80-column text is 640 pixels wide. Ports 3B0h-3DFh are the VM's model;
multi-byte accesses decompose low byte first; bytes past 3DFh read FFh.

Retrace (superseded 2026-10-12, see below): the presenter's frame clock
was 33 host ticks, the last three vertical retrace (status bits 3 and 0);
in the active period bit 0 alternates on successive reads as a
horizontal-blanking stand-in.

Retrace after wolf3d-qemu-t23 (2026-10-12): Wolf3D spun in id's
`VL_SetScreen` (WOLFSRC/ID_VL_A.ASM,
<https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_VL_A.ASM>),
called as `VW_SetScreen(0x8000,0)` from `SignonScreen` before `SD_Startup`
(WOLFSRC/WL_MAIN.C), so its `TimeCount` escape cannot fire; the loop then
needs five successive 3DAh reads with display disabled (bit 0) and no
vertical sync (bit 3), "the bottom of the screen", which the old model never
produced twice in a row. Input status 1 now ends every frame of the
programmed timing with the vertical non-display region (FreeVGA: bit 0 is
the inverted display enable, set in horizontal and vertical blanking; bit 3
the vertical sync): 16 reads display disabled, 4 reads sync, 16 reads display
disabled, then the active period with bit 0 alternating. Frames are due at
the programmed vertical total (CR06 with CR07 bits 0 and 5, plus 2) at the
31.469 kHz line rate of the standard VGA modes, on the host tick (independent
of the guest's IF): 70.09 Hz for 449-line modes, 59.94 Hz for 525-line
modes. The read floors make each region visible whatever the trap cost.
Only 3BAh/3DAh reads advance the sequence. The presenter cadence (33 ticks)
is unchanged.

Wolf3D play stage (wolf3d-qemu-t23 with the retrace fix, 2026-10-12): no
flip in the 3 s window and 1.14 million emulated aperture stores. Wall
strips wider than one pixel set multi-plane map masks (WOLFSRC/WL_SCALE.C
`mapmasks1-3`: 3, 7, 15, 6, 14, 12) that the single-plane direct window
cannot carry, and the compiled scalers (`BuildCompScale`: `mov al,[si+d8]`,
`mov es:[di+d16],al`, ..., `RETF`) store one byte per instruction, so every
stored byte was a #PF. Decision: after a V86 aperture #PF the trap path
keeps emulating the following instructions with the same decoder
(`dosvm_vga_burst`) while they touch the aperture (at most 4 in a row
without), up to 1024; it stops before anything the decoder does not handle,
any store outside the aperture, TF, an STI/MOV SS shadow or a deliverable
interrupt, so the guest sees the sequential result. Host replay of one view
through `dosvm_trap` (tests/host/dosvm_workloads_test.inc): near wall, 4-pixel
strips, 11552 -> 76 #PF per frame; mid wall, 2-pixel strips, 15200 -> 152;
1-pixel strips stay on the direct window (304 remaps, 0 #PF); planes
identical to the reference. Flips were already counted (CRTC 0Ch data
writes, byte OUTs as in `ThreeDRefresh` or OUTW); the probe now starts its
3 s flip window only once flipping is steady (3 flips in one second, within
90 s of the last menu key) and reports `start_ms`, `burst` and per-window
`faults`/`maps`.

Fonts: the synthetic ROM (F000:2000 8x16, F000:3000 8x8, F000:4000 8x14) is
a deterministic conversion of the kernel's Inconsolata console bitmap
(OFL-1.1) for printable ASCII. CP437 accented letters use their base letter,
shades/blocks/bullets and all 40 box-drawing characters (single, double and
mixed, with closed double corners) are drawn geometrically, other codes show
`?`. 8x14 and 8x8 cells OR the 16-row source rows they cover, which keeps
one-pixel strokes legible in 80x50. No firmware font is copied. (This
supersedes the partial's "non-ASCII use ?" and "sample alternate rows".)

Presenter: a fixed 640x480 XRGB surface, the largest integer scale that fits,
centred, black borders; no fractional scaling or aspect correction. Dirty
granules are taken and cleared atomically at frame start, so stores during a
frame stay marked; scanout-register/DAC changes, geometry changes and font
(plane 2) changes redraw everything; blink (500 ticks) and cursor (250 ticks)
phases redraw only phase-sensitive scanlines. At most one frame per 33 ticks.
A scanout the models cannot render or that exceeds 640x480 (custom/9-dot)
produces one black frame and the VM keeps running. The host harness proves
pixel equality with the retained `cvp_present` for all five pattern stages
at two phases.

Native authority (execution-abi.md, F3-02 amendment): syscall 72 with a
136-byte record; the authority is a spawn-time process flag set only for an
exact `/Programs/dosbox` whose parent is the supervisor or owns both a display
and an input grant. One VM per process, keys bound to the owner, generation
validation, a creation reservation against concurrent creators, closed
error set. Owner death reaches the supervisor's collection pass, which
destroys the VM through the idempotent f3-00 path; a binding busy in its
owner's syscall is collected at that syscall's exit. CLOSE forces only a VM
that is still running, so a guest EXIT reports no data loss. Window VMs own
the record name `vm-window`; the surface is not charged to VM residency (it
is already outside the free pool used by admission).

Input (after the f3-01 merge, 2026-10-11): `window_focus` and `window_input`
in `dosvm_client.c` call `dosvm_input_focus` and `dosvm_input_event`. A
focused KEY event becomes set-1 bytes in the VM's 8042 and IRQ1; the
synthetic INT 9 (`dosvm_kbd_bios`) fills the BIOS buffer; MOTION and BUTTON
drive the INT 33h model, so mouse callbacks work in the window; TEXT is
ignored; focus loss and RESYNC send break codes for held keys and release
buttons. The interim BIOS-word adapter is gone from the client. Its tables
moved into `dosvm_kbd_bios`, because the f3-01 translation did not cover
these BIOS words (HelpPC table): Shift/Ctrl/Alt F1-F10, F11/F12 (85h-8Ch),
Alt digits and `-`/`=` (78h-83h), Ctrl navigation (73h-93h), Ctrl Enter,
Backspace, `2`, `6`, `-`, `[`, `]`, `\`, Shift-Tab (0F00h), the Num Lock
keypad (digits and `.`), keypad `-`, `+`, Enter (1C0Dh) and `/` (352Fh). Num
Lock and Scroll Lock now toggle BIOS shift bits 20h/10h instead of queuing
a key. Letters, Shift, Caps Lock and Ctrl/Alt letters keep f3-01's results.
The text-mode INT 33h cursor appears in the probe text view
(`dosvm_text`); the window frame does not draw it, the desktop pointer is
the visible cursor.

ROM map: F000:0000-0FFF interrupt stubs, F100:0000 EMS entry and F000:1020
INT 33h continuation (both in page F1000h), F000:2000 8x16, F000:3000/3400
8x8 and F000:4000 8x14 fonts (F2000h-F4DFFh). The fonts stay clear of
F0000h-F1FFFh.

`/Programs/dosbox` (apps/dosbox, MIT): one process, one VM, one window titled
with the VM name (`--name=`, default `CiukiDOS`), protocol v2 close request
→ CLOSE and exit, VM stop → window close, focus/key/motion/button forwarding,
damage at the kernel cadence. The desktop passes `--channel-fd=3` to it and
gains `--launch=<program>`, which starts one `/Programs` menu program through the
menu-click path (used by `f3:vm-window`). Adding `/Programs/dosbox` grows the
bottom-anchored Ciuki menu by one row, so the f2-30 `desktop-shell-*`
scripted click on Demo moves from `y-169` to `y-197` (1024x768). With the
f2-31/32/33 desktop batch (`/Programs/files`, `/Programs/ciuknote`, `/Programs/tasks`) the
offsets follow the formula in f2-acceptance.md, section F2-33 (Demo: -253).

### Probes, cases and golden digests

`f3:vm-video run=<8hex>` (no suffix; sweep after `vm-memory`): one 8 MiB VM
boots to the prompt (`case=boot`, `case=prompt` with an informational prompt
digest), types `\VMVIDEO.COM` and, for each of five stages, waits for the
guest line `VMVIDEO s f`, for INT 16h to block the VM, then hashes the whole
640x480 presenter frame at tick 0 (`case=frame stage mode width height scale
text flags digest`). Stage 6 restores text; `case=return`, EXIT, then
`case=cleanup` against the pre-probe ledger. Cases `vm-video-qemu-t23/e500/
min128` pin mode, geometry, flags (stage 1: retrace edges and DAC read-back;
stage 3: retrace) and the digests:

| Stage | Frame | SHA-256 |
| --- | --- | --- |
| 1 | mode 03h, 80x25, 640x400 | `0893476bb8b2355dd506ca57bba164a2a7b607df49280278f1b3f857e95424de` |
| 2 | 80x50 (8x8 font), 640x400 | `c4ac397792548fb0228a889fd7567275dd8148637fa48405fef5386851fdf12c` |
| 3 | mode 13h + DAC 40h-7Fh, 320x200 x2 | `c019f0109e614d8ae01d35638587b23d92148b8d442dfd0deba73fe3a6df527c` |
| 4 | Mode X 320x240 x2 | `a6d61fa11188a1e81da20dc5f018ee49c264fa0beb004587970b061058d3742a` |
| 5 | mode 12h, 640x480 | `81daa6ab021444158a8c45f09c72de3b13ee45d7ddf22c920a65ac496a4ec9c2` |

Golden procedure: `tests/host/dosvm_video_test.inc` (`video_stage`) repeats
each stage's INT 10h calls, port writes and VRAM stores of
`src/probes/dos/vm_video.asm` in order on a real `dosvm_create` VM and
hashes `dosvm_frame_digest`. On a deliberate pattern, font or presenter
change, run the harness, copy the printed `VIDEO golden` digests into
`video_golden[]` and into the suite; `tests/host/test_dosvm.py` fails unless
the two agree. `DOSVM_VIDEO_DUMP=<dir>` writes the five frames as PPM.
The digests assume that CIUKIDOS leaves BDA 0489h (palette-loading and
grey-scale flags) clear; a QEMU mismatch on stage 1 points there first.
`VMVIDEO.COM` has no guest HLT wait: stage keys use INT 16h AH=00h, which
blocks in the monitor, and retrace is a bounded 3DAh poll, so the f3-01
wait-loop rule (test with IF clear, sleep with `STI; HLT`) has no site in
it. A stage or prompt timeout in either probe adds `group=vm_devices` (IRR,
ISR, IMR, 8042 queue, held keys, IRQ0/IRQ1 deliveries, last vector, PIC
bases from `dosvm_device_info`) before the `group=screen` rows, which are
owned by the running probe.

`f3:vm-window run=<8hex>` (sweep after `vm-video`): the probe spawns
`/Programs/desktop --launch=dosbox` under a controller owner, waits for the bound
window VM and its prompt (`case=window`), emits `ARM action=window_input`
(the runner moves the pointer off the window and types `\VMVIDEO.COM` with
QMP keys; a runner request never gets kernel typing: the probe waits up to
120 s for stage 1 and otherwise fails with the device diagnostics; only a
BOOT.CFG sweep (`probes_operator_mode`) types it itself after 30 s and
records `typed=kernel`, which the QEMU cases refuse), hashes stage 1
(`case=frame stage=<latched stage> typed=window`; any stage other than 1
fails the probe), emits `ARM action=window_screen digest=` and holds stage
1 for 6 s (a stage change during the hold fails the probe). The runner's new
`window_screen` check takes a QMP screendump 2 s after that ARM and requires
the composited client region (origin 23,76: first window at 20,52 plus the
3-pixel border and 24-pixel title) to hash, as XRGB rows, to both the
suite's stage-1 golden and the ARM digest. The probe then finishes the
stages and types EXIT: `case=closed` requires reason 0, the window binding
released and `/Programs/dosbox` exited; `case=cleanup` requires the VM ledger and
the desktop object ledger (descriptions, surfaces, pages, channels,
messages, grants) back at baseline. Cases: `vm-window-qemu-desktop-2002`
and `vm-window-qemu-desktop-1998` (needs a 24/32-bit desktop of at least
663x556).

First QEMU run of `vm-window-qemu-desktop-2002` (2026-10-11, lead
evidence): no `VMVIDEO` line in 30 s although the runner delivered all 31
batches; after the fallback typed the command, VMVIDEO ran stages 1-4 at
once and the probe hashed the stage-4 Mode X frame (digest a6d61fa1…, the
stage-4 golden) under the label `stage=1`; the screendump showed the kernel
console drawn over the whole desktop background. Causes and fixes:

- Window keys never reached a blocked shell. The shell reads a line with
  INT 16h AH=00h, which `dosvm_wait` served by sleeping until the BIOS
  buffer (0041Eh) held a key; window keys reach only the virtual 8042 and
  IRQ1, and the guest INT 9 that moves them into the buffer cannot run
  while the monitor sleeps inside the INT FEh service. The keys stayed
  queued until the fallback wrote the BIOS buffer directly
  (`dosvm_keyboard`); the queued bytes then became BIOS words after the
  command and three of them advanced VMVIDEO. A BIOS INT 16h read enables
  interrupts and loops until the keyboard ISR has stored a key (Art of
  Assembly, ch. 20.4, INT 16h replacement: "We need to turn interrupts on
  here (so the kbd ISR can place a character in the buffer if one is
  pending) ... BIOS always forces interrupts on",
  <https://www.cs.yale.edu/flint/cs422/doc/art-of-asm/pdf/CH20.PDF>).
  Decision: with the BIOS buffer empty, a key byte in the 8042, IRQ1
  requested and an interrupt deliverable (`dosvm_kbd_irq_ready`), AH=00h/10h
  re-executes its own INT FEh at F000:0160 with the guest IF set; the wait
  also returns when such a byte arrives. The IRQ is delivered first, the
  guest INT 9 (ROM stub or a DOS/TSR hook) fills the buffer and the retry
  returns the key. Kernel typing and every other wait are unchanged.
  Evidence: `test_client_typing` in `tests/host/dosvm_video_test.inc` (the
  runner's 30 events through syscall 72 into a guest blocked in AH=00h,
  production ROM stubs through `dosvm_trap`) and the guest runner's
  `keys:\VMVIDEO.COM VMVIDEO` row on the real DOSBOOT.IMG (stage 1, flags
  3, digest 0893476b…, then one stage per Enter); both fail on the previous
  code (the guest runner reports 30 bytes queued, BIOS buffer empty, stage 0).
- The console drew over the desktop. `kputs_raw` writes every log line and
  record to the screen console, whose scroll redraws all rows from history;
  the presenter arbitration in `fbdev.c` only serialises single lines. The
  production desktop repaints damage only (clock, window damage), so the
  background stayed console text. The F2 gate desktop hid this by
  repainting the whole frame on every interaction pass and after each
  report. Decision: while a process holds the display grant
  (`grants_install` to the last `grant_release`) the console keeps history
  and serial but draws nothing; the next line after the release repaints
  the screen from history. The operator prompt and evidence pages draw as
  deliberate overlays, and the panic paths reclaim the console first.
  F2 checks are unaffected: their pixel digests are compared between
  records or still marked PIN-FROM-FIRST-RUN, and the desktop_screen check
  reads desktop pixels only.

### Host and build evidence (worktree, 2026-10-11)

- `bash scripts/test/host_kernel_tests.sh`: PASS (after the f3-01 merge). DOS VM harness 1517061
  checks (ASan/UBSan) including the f3-00/f3-03 cases, VGA BIOS/ports/ROM
  fonts, aperture emulation, guest #DE reflection and trap CR2, presenter damage/phases/custom
  scanout, five goldens with retained-presenter equivalence and the window
  boundary (authority, record validation, keyboard adapter, owner death,
  allocation/storage/boot failures); migrated `test_virtual_vga.c` 173117
  assertions; ABI oracle 805 checks; record-scope PASS.
- `python3 -m unittest tests.host.test_dosvm tests.host.test_vga_x86`, `tests/host/desktop`,
  `test_sweep.py`, `test_loader_*.py`, `test_image_payloads.py`,
  `tests/loader_stub/test_static.py`: PASS. `test_runner.py`,
  `test_physical.py` and `test_desktop_gate.py` share the QEMU lock and were
  left to the lead.
- `make kernel` (clang i686, -Werror, FPU audit) and `make sdk` / `make
  desktop` (desktop, demo, dosbox inspected): PASS. `DOSBOOT.IMG` builds with
  `VMVIDEO.COM` (T1 DOS: 12 payloads read back).
- `tests/host/test_vga_x86_shim.c` (unchanged) is driven by the new
  `tests/host/test_vga_x86.py` with hand-derived vectors (stores, MOVS cycle
  order, read-modify-write flags, REP restart, unsupported, divide error,
  V86 64 KiB limit). The archived Unicorn differential script is not
  restored: Unicorn is not a project dependency.

## f3-03

Research checked before implementation, 2026-10-11: [XMS 3.0](https://www.phatcode.net/res/219/files/xms30.txt),
[LIM EMS 4.0](https://www.phatcode.net/res/218/files/limems40.txt),
[QEMU 9.2 i8042](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/input/pckbd.c)
and [ACPI address-map interface](https://uefi.org/specs/ACPI/6.5/15_System_Address_Map_Interfaces.html).
The existing monitor supplies checked private copies and synthetic interrupt
stubs; its allocator owns each mapped page. EMS aliases therefore require
explicit removal before freeing a pool block or destroying the address space.

Decisions: keep the 32 MiB commitment, 8 MiB low-memory allowance and 32 MiB
headroom. The pool is `00110000h` onwards, limited to commitment minus 8 MiB
and at most 24 MiB; EMS has an 8 MiB sub-cap. Allocations use NORMAL pages
and contiguous guest addresses, never contiguous host addresses. The pool is eagerly backed at creation so synthetic BIOS RAM is directly
usable; resident backing and handle usage are reported separately. Both services
share the same allocator; failed growth is transactional. XMS sizes remain
KiB, EMS pages 16 KiB, physical backing 4 KiB. No extra 64 KiB rounding is
introduced into XMS. Firmware E801 high memory alone uses 64 KiB units.

Default frame: E000h; explicit D000h configuration is supported. UMBs stay
within the contract's C800h–DFFFh ceiling and exclude the selected frame.
Reserve C800h's first page for the far-call XMS stub, leaving C900h onwards
for page-rounded UMB allocations. HMA remains wholly available to its single
XMS owner. CIUKIDOS INT21/58h remains unchanged: no DOS-owned UMB MCB chain,
link=0, only conventional strategies 0–2; upper strategies/linking fail.
This follows the actual `int21` implementation rather than advertising a DOS
arena that AH=48h cannot use.

LIM's reserved 49h/4Ah fail. OS/E-only hardware, alternate-map/DMA and warmboot
functions are subject to the specification's access-denied policy (A4h);
there is no physical EMS hardware or VCPI grant. The application interface,
including partial maps and alter-map jump/call, is implemented independently.
Unsupported XMS extensions in 88h–8Fh return 80h. All memory BIOS answers are
synthetic descriptions of this VM's pool; host firmware/allocator addresses
are never returned. BIOS 87h validates both descriptors and complete extents
before copying and preserves virtual A20.

Host and build results and lead qualification are recorded below after
verification. These decisions do not constitute QEMU or hardware evidence.

### Probe `f3:vm-memory` (lead implementation after Codex's usage limit, 2026-10-11)

Selector `f3:vm-memory run=<8hex>` (platform override as in F1/F2, no safe
suffix; lead change of 2026-10-11 in the loader, the kernel parser and the
runner model so the qemu-e500 profile can run F3); the sweep lists it
after `vm-boot`. The controller creates one VM with the default 32 MiB
commitment and `probe = "vm-memory"` (the record owner of its VMs), boots it to the prompt (`case=boot`:
`ready`, `commitment`, `pool_quota`, `pool_used`, `free_paras`), types
`VMMEMORY.COM` and waits up to 60 s for the kernel's verdict latch, written
by the E9 line `VMMEMORY PASS`/`VMMEMORY FAIL` (`case=conformance`: `done`,
`pass`, `xms_calls` and `ems_calls` as function bitmasks, `pool_used`,
`guest_records`, `dropped`, `error`). It then creates a second VM with the
same configuration: `group=admission` is emitted by `dosvm_create` for both
VMs (free, reserved, resident, outstanding, request, headroom, admitted);
`case=second` records `admitted` and the error; an admitted second VM boots
to the prompt (`case=second_boot`) and exits; a refusal with `-ENOMEM` is an
explicit outcome, any other error fails. `EXIT` ends the first VM; `case=cleanup`
compares the ledger with the pre-probe snapshot. `VMMEMORY.COM` is assembled
from `src/probes/dos/vm_memory.asm` into `DOSBOOT.IMG` (eleven payloads).
Suite `f3-vm.json`: `vm-memory-qemu-t23/e500/min128`. The first QEMU runs
(worktree image f47a4d78…, 2026-10-11) admitted the second 32 MiB VM on all
three profiles (min128: 116 MiB free before the first VM, 91 MiB free with
32 MiB reserved and 25 MiB resident before the second), so `admitted` is
pinned to 1 with a successful `second_boot` on every profile; conformance
reported `xms_calls=007fffff ems_calls=3fffffff` and both VMs exited cleanly.

### Lead findings while qualifying f3-03 on QEMU (2026-10-11)

The f3-00 row had been merged with its QEMU evidence pending, and no DOS VM
had booted on an integrated image before this work. Two blockers were found
and fixed in `wt/f3-vm-memory`:

1. **Lock order.** `dosvm_disk_open` took the per-VM `disk_lock` kmutex and
   then the ATA channel mutex introduced by f4-02; `kmutex_lock` panics on a
   held mutex whose address is not below the new one. The VM disk path now
   serialises its own transfers with a per-VM `kwait` gate (`disk_busy`,
   `disk_wait`, bounded 1 s waits) and no longer holds a kmutex across the
   storage call. `kmutex_lock` logs `[mutex] held=%p new=%p task=%s` before
   panicking.
2. **ROM write at the boot splash.** `stage1_show_boot_splash` issues
   INT 10h AX=4F02; the synthetic BIOS answers 014F; the kernel's INT 10h
   hook falls back to its local VBE emulation and allocates the 192 KiB
   backing store with no active PSP. `int21_mem_arena_start` returned 0 in
   that state, so `int21_mem_init` wrote the first MCB at FFFF:0000. Real
   firmware ignores writes to ROM; the VM's ROM pages are read-only, so every
   VM stopped with `vector=14 address=000ffff0 error=7`. The arena now starts
   at `DOS_HEAP_USER_SEG` without a PSP. In the same installer the
   INT 60h–67h default-IRET loop no longer replaces INT 67h when the handler
   segment carries `EMMXXXX0` at offset 000Ah (LIM EMS 4.0 §3.3); otherwise
   `VMMEMORY.COM`'s signature check cannot see the synthetic EMM.
3. **Working directory.** `init_shell_default_dirs` creates `\SYSTEM` and
   `\APPS` and the shell starts in `C:\APPS`; COMMAND_COMPAT resolves bare
   names in the working directory only, so `AUTOEXEC.BAT`'s `DOSREADY.COM`
   was never found and `dos_ready` never latched. The DOS image and the
   probes now name the payloads absolutely (`\DOSREADY.COM`, `\VMPORT.COM`,
   `\VMPRIV.COM`, `\VMMEMORY.COM`).
4. **OUT operand width.** `v86_emulate` hands the port handler the whole
   EAX for every OUT; `dosvm_io` compared that value with command bytes
   (`D0h`, `DDh`, `DFh`, `FEh`, the control port's zero), so `out 64h, al`
   with a non-zero AH never latched the D0h reply (VMMEMORY tag A7) and the
   shell's exit write to port EAh would have faulted the VM. The handler now
   uses the written byte only; the host harness drives the same writes with
   a dirty AH.

Host coverage added for item 6 of the directive (`tests/host/dosvm_test.c`,
`test_memory_services`): a VM with a 4 MiB pool checks eager backing and the
unmapped frame windows, XMS 00h–12h/88h/8Fh (HMA ownership, A20 counts and
94h, free queries, handles, lock/unlock, growth that relocates past an
occupied neighbour and keeps its bytes, shrink, first-fit reuse zeroed), the
move validator (both directions, A3h–A7h), UMB 10h–12h (B0h/B2h, page-rounded
arena C900h–DFFFh), EMS 40h–51h (page-table aliases per window, map errors
8Ah/8Bh/83h, handle list, save/restore 8Dh/8Eh, reallocation relocating and
trimming windows, release), denied and unknown functions, destruction with
aliases still mapped, and a pool-backing failure during creation.

The EMS entry vector is F100:0000 (stub page F1000h: INT FEh, IRET, name
`EMMXXXX0` at offset 000Ah): LIM EMS 4.0 detection reads the name at the
handler segment's offset 000Ah, which F000:1000 could not satisfy, and
F000:1000 collided with f3-01's INT 33h callback continuation (f3-01 review).

Fault diagnostics added to the controller records: `group=vm_stop` carries
`address` (CR2 read before interrupts are re-enabled), `error` (page-fault
error code) and `code` (the eight bytes at the faulting CS:IP); a stopped VM
with `reason=2` also emits `group=vm_fault` (AX…BP, DS, ES, SS:SP) and three
`group=vm_stack part=N` rows with sixteen stack words each. Guest addresses
map to the NASM listings: the stage-1 loader runs at linear 8000h, the DOS
kernel is loaded at 9000h and relocated to 3000h (listing offset + 3000h).

## f3-05

Per-VM Sound Blaster 16, OPL3 and virtual 8237A state, the VM audio timeline
and the interface F4 consumes (directive f3-05). Implementation and host
evidence only; the QEMU `f3-vm` audio-state cases belong to the lead.

### Research checked before implementation (2026-10-11)

- Creative, [Sound Blaster Series Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf)
  (contract source S5). Chapter 2: reset (write 1 to 2x6h, 3 us, write 0,
  poll 2xEh bit 7, read AAh), 2xCh bit 7 clear = ready, 82h interrupt status
  (bit 0 8-bit DMA/SB-MIDI, bit 1 16-bit DMA, bit 2 MPU-401), acknowledgement
  by reading 2xEh (8-bit) or 2xFh (16-bit), 80h IRQ select (2/5/7/10, one
  bit), 81h DMA select (0/1/3 and 5/6/7, one bit each; no 16-bit bit means
  16-bit data on the 8-bit channel). Chapter 3: time constant
  `65536 - 256000000/(channels x rate)` (high byte), auto-init double
  buffering, termination by exit command or by programming a single-cycle
  transfer that "continues" at the end of the current block, high-speed mode
  ignores commands until reset and its reset restores the earlier parameters,
  4.xx rates 5000-44100 Hz. Chapter 4: CT1745 register map and defaults
  (30h-35h level 24 = -14 dB; 36h-3Ah 0; 3Ch 1Fh; 3Dh 15h; 3Eh 0Bh; 44h-47h
  8 = 0 dB; 04h/22h/26h/28h/2Eh/0Ah are CT1345 views of the new registers).
  Chapter 6: command matrix by DSP version; 41h/42h 5000-45000 Hz; Bxh/Cxh
  bit 3 A/D, bit 2 auto-init, mode bit 4 signed, bit 5 stereo, length in
  samples - 1; D1h/D3h on 4.xx only set the D8h status latch. Appendix A:
  FM at 2x0h-2x3h, 2x8h/2x9h and 388h.
- DOSBox Staging [soundblaster.cpp](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/audio/soundblaster.cpp)
  and [opl.cpp](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/audio/opl.cpp),
  behaviour only (no code taken): SB16 82h reports 20h in its reserved bits,
  E1h returns 4.05, E3h copyright string, argument counts of undocumented
  commands, the CT1345-to-CT1745 level mapping `2n+1`, reset clears pending
  causes; OPL timers start aligned to their 80/320 us clock, a mask clears
  its flag, RST clears both flags, one address latch with array 1 reachable
  only for 105h while NEW is clear, OPL3 status low bits 0 (OPL2 06h).
- Intel [8237A datasheet 231466-005](https://pdos.csail.mit.edu/6.1810/2004/readings/hardware/8237A.pdf):
  base and current registers written together byte by byte through the
  flip-flop, TC when the count rolls from 0 to FFFFh, autoinitialize reloads
  and keeps the channel unmasked, otherwise the mask bit is set, status bits
  0-3 TC (cleared on read) and 4-7 requests, master clear, clear mask, write
  all mask bits, temporary register, illegal reads. [QEMU i8257](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/dma/i8257.c):
  page ports, word channels shifted by one with page bit 0 ignored.
- Linux [snd_opl3 detection](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/drivers/opl3/opl3_lib.c):
  status E0h clear after reset, C0h after timer 1 = FFh started for >= 80 us,
  timer resolution 80 000 ns and 320 000 ns, signature 06h = OPL2.
- Repository: legacy `src/vm/guest_peripherals.c` (DMA/SB/OPL models of the
  0.8 line, migrated as reference) and its `test_guest_peripherals.cpp`
  audio cases (migrated into the new harness); `session_audio.c`
  `cvaudio_dma_slice` (block-boundary slicing, now the timeline rule below).
  CIUKIDOS exports `BLASTER=A220 I7 D1 H5 T6`, so the model's factory setting
  is base 220h, IRQ 7, DMA 1, HDMA 5, OPL3 at 388h.

### Decisions

Files: `src/kernel/vm/dosvm_sb16.c` (DSP, mixer, timeline, PCM queue, port
dispatch, F4 calls), `dosvm_dma.c`, `dosvm_opl.c`, private state
`dosvm_audio_state.h`, public interface `src/kernel/include/ciuki/dosvm_audio.h`.
State lives in `struct dosvm` (about 3.7 KiB per slot); the PCM ring (4096
stereo frames, four pages) and the event ring (512 events, two pages) are
allocated at creation, charged to `resident` and to the ledger's pages, and
freed by the single release path. Ports: 00h-0Fh, 80h-8Fh, C0h-DFh,
220h-22Fh, 388h-38Bh, registered with `dosvm_port_register`; the handler runs
under `irq_save` like the PIT and 8042 because `dosvm_tick` shares the state.
Wider CPU accesses are split into byte cycles, as on the 8-bit ISA bus (the
mixer `out dx,ax` index/data idiom). No physical port, DMA or audio device
is touched.

DSP: version 4.05. Implemented: reset/status, E0h/E1h/E3h/E4h/E8h, D1h/D3h/
D8h latch (never gates output), 10h direct DAC (timestamped DAC events),
14h/91h single-cycle and 1Ch/90h auto-init 8-bit, Bxh/Cxh 8/16-bit signed/
unsigned mono/stereo single/auto-init, 40h/41h/42h (41h clamps to 5000-45000;
a time-constant rate is halved for stereo; any rate is capped at 45000), 48h,
80h silence, D0h/D4h, D5h/D6h, D9h/DAh, 45h/47h (continue: cancel a pending
exit), F2h/F3h forced causes, SB Pro output stereo through mixer 0Eh bit 1
for the legacy 8-bit commands (a DOSBox-compatible choice; the CT1745 map has
no 0Eh). High-speed mode ignores commands until reset; MIDI UART mode
discards writes until reset; those resets restore the earlier parameters,
an ordinary reset sets 22050 Hz and speaker off. A new transfer during
auto-init applies at the end of the current block. Not advertised, counted
as `unsupported`: recording (Bxh/Cxh with A/D, 24h/2Ch/98h/99h start nothing;
direct 20h answers the midpoint 80h so a polling reader cannot hang), MIDI
input, ASP (0Fh answers 00h), E2h and undocumented commands. Creative ADPCM
(16h/17h/74h-77h/1Fh/7Dh/7Fh) keeps DMA, block timing and IRQs and produces
silence flagged `UNSUPPORTED`: no decoder is advertised in F3.

Mixer: CT1745 defaults above; 22h/04h/26h/28h/2Eh/0Ah views; register masks;
80h/81h accept exactly one valid bit (otherwise unchanged, counted); 82h is
read-only `causes | 20h`. The IRQ line is the OR of the 82h causes: a cause
latched while the line is low raises one edge through `dosvm_irq_raise`
(f3-01 PIC), none while it is high. Reading 2xEh/2xFh lowers the line but,
as in f3-01, cannot erase an IRR bit already latched.

Timeline: frames of a 48 kHz clock per VM, advanced by `dosvm_tick` (48 per
host millisecond) whatever the guest IF, IMR or ISR state. DSP consumption
uses exact integer phase (`rate` per timeline frame, `48000/channels` per
sample, `48000 x samples` per ADPCM byte), so block boundaries have exact
nominal times and segment start stamps are reproducible. The block-boundary
rule (superseded by f3-40, [research](../validation/f3-40-sb16-irq.md)):
at a boundary the DSP closes the segment, latches the cause, raises an edge
only if the line was low, and immediately continues auto-init DMA on its
nominal timeline. An already asserted line increments `irq_merged` and
`late`; the following segment is flagged `LATE`, and a block started after
two unserviced boundaries is flagged `STALE`. `waiting` describes a latched
cause only. Status reads clear their matching cause without advancing DMA;
DMA wrap replays unrefilled halves with no tempo change. Each call processes at most 64 ms of
DSP time; backlog beyond one second is dropped and counted. A DMA channel
that does not answer (masked, controller disabled, cascade) stalls the DSP
and the elapsed time is lost, as on the card; a non-read transfer type or an
address outside the VM stops the transfer locally (no IRQ, `last_error`,
DMA fault counter), the DSP and the VM continue.

8237A: addresses resolve only through the owning VM's linear view (as
Windows 95 VDMAD virtualises V86 DMA, including the VM's A20 state); 8-bit
channels wrap inside their 64 KiB page, word channels address words with
page bit 0 ignored inside 128 KiB; decrement mode, verify (no memory cycle,
open-bus FFh), write/illegal types refused; reads go through `dosvm_read`
in bounded chunks.

OPL3: 512-byte register file, single address latch, NEW (105h) and pair
(104h) state, timers 1/2 at 80/320 us with free-running prescaler alignment,
mask and RST semantics, status bit 7 when a flag is set. No PIC line: the
SB16 and AdLib do not wire the OPL IRQ output, so "IRQ" is the status bit.
Timer and status time are the VM's microseconds plus a virtual ISA I/O time
of 1 us per OPL access inside the current millisecond, capped below the next
tick, so status-read delay loops (AdLib/Linux detection) observe time.
Every data write is queued as a 16-byte event (VM generation, timeline frame
including that sub-millisecond I/O time, register, value). Without a
consumer, events are counted as discarded while the register file stays
authoritative; on overflow the next event is preceded by a `GAP` marker and
F4 resynchronises from `dosvm_audio_opl_registers`.

F4 interface (`ciuki/dosvm_audio.h`): one consumer token per VM
(`dosvm_audio_attach`/`detach`; VM stop or a new attach revokes it);
`dosvm_audio_fetch` returns frames of one segment only (signed 16-bit
stereo at the source rate, with start frame, sequence, rate, source format,
offset, cause and flags `COMPLETE/SILENCE/UNSUPPORTED/OVERRUN/LATE/STALE/
ABORTED`), never across a block boundary, partial segments continue by
offset; `dosvm_audio_events` drains events in write order;
`dosvm_audio_info` reports counters, configuration, the cause latch, mixer
master/voice/FM levels and OPL timer stamps. Without a consumer, frames are
counted as discarded; a full ring drops new frames (`OVERRUN`, counted).
The ledger's `devices` count is unchanged (3): the vm-boot predicates pin it.

### Probe `f3:vm-audio-state`

Selector `f3:vm-audio-state run=<8hex>` (platform override as for the other
F3 probes), fourth in the F3 sweep (after vm-boot, vm-memory, vm-devices).
The controller boots a witness VM and the test VM (8 MiB each, records named
`vm-audio-state`), attaches as the F4 consumer and types `\VMAUDIO.COM`
(`src/probes/dos/vm_audio.asm`, in `DOSBOOT.IMG`). The payload parses
BLASTER from its environment, publishes stage 1 (detection) and then runs one
stage per command word in the mailbox at 0000:0600 (2 8-bit single-cycle
ramp; 3 64-unit auto-init double buffer, 32 blocks with ISR refill and DAh
from the ISR; 4/5 pause, 50 ms hold, continue, exit; 6 16-bit signed stereo
auto-init on HDMA with 2xFh acknowledgement and D9h; 7/8 CLI for 100 ms then
STI; 9 OPL script; 10 DMA page F0h outside the VM; 11 restore IVT/IMR and
return to DOS). Records (`DATA case=`): `detect`, `mixer`, `dma8_single`,
`double_buffer`, `pause`, `continue`, `dma16_stereo`, `if_clear`,
`if_restore`, `opl`, `opl_state`, `dma_fault`, `exit`, `locality`,
`cleanup`; each carries `ok` and the raw values the suite pins (fresh
blocks and their exact nominal spacing: 384 timeline frames for 64 units at
8000 Hz, 256 for 64 frames at 12000 Hz; services; IF-clear boundaries
against host time, at most one edge, one pending request, no delivery;
the 30 OPL events in order with generation, monotonic stamps inside the
stage window; timer flags 20.4-20.48 ms and 81.6-81.92 ms after their start;
locality of the witness). Stage timeouts emit `stage_timeout` and
`stage_audio` diagnostics (two records, each under 256 bytes).
Suite `f3-vm.json`: `vm-audio-state-qemu-t23/e500/min128`, timeout 180 s.

### Host and static verification

`tests/host/dosvm_audio_test.inc` (in the production DOS VM harness, ASan/
UBSan): detection and every mixer register path including the word idiom;
the migrated cvgp single-cycle case (four samples, speaker off still plays,
82h, 2xEh/2xFh, forced F3h, the PIC keeps its edge, ADC refused, address
outside the VM and write-type channel fail locally); every advertised PCM
layout (8/16-bit, signed/unsigned, mono/stereo, single/auto-init) decoded
exactly, 16-bit on the 8-bit channel, SB Pro stereo with a time constant,
silence, ADPCM timing, direct DAC events, high-speed reset restore; the F4
regression (64-unit auto-init double buffer over 2048 units: 32 sequential
fresh blocks and 32 eligible services although the fake output clock
advances in 10 ms steps, consumer fetching 50-frame quanta, spacing
139/140 frames) and its counter-case without service (nominal pace, LATE/
STALE, one edge, IF never forced); IF clear, IMR masked and ISR in service
for 100 ms (exactly ten boundaries, at most one edge, one pending request,
STI/MOV-SS shadow defers, exactly one delivery); pause/continue/exit/45h/
new command at the block end/abort on reset; the 8237A register set, wraps,
autoinit, decrement, verify, cascade, disabled controller, word channels,
faults; OPL detection, migrated timer case, mask/RST, NEW/array addressing,
SB16 mirrors, event order/generation/stamps, overflow and GAP, register
snapshot, two chips independent; queue limits (ring overrun, segment ring,
stale tokens, VM stop), backlog drop and drain. A mutation that disables the
boundary wait fails the F4 regression. Creation-failure injection now covers
283 allocation sites (six new audio pages) with exact teardown.

Results on this worktree (2026-10-11): `bash scripts/test/host_kernel_tests.sh`
rc=0 (dosvm harness 196 696 checks, largest record 212 bytes);
`make kernel` green; `python3 scripts/build_dos_image.py` T1 DOS PASS with
13 payloads; `tests.host.test_dosvm` (including the new suite fixture),
loader, loader-model and sweep tests pass; the loader still assembles to its
fixed 24 KiB.

Cross-check of the payload against an independent SB16/OPL3 model: a scratch
copy of `VMAUDIO.COM` without the controller handshake (mailbox dumped at the
end; scratch files removed) ran under DOSBox 0.74-3 (`sbtype=sb16`, A220 I7
D1 H5). All eleven stages completed; BLASTER parsing, reset AAh, version
4.05, E0h/E8h, speaker FF00h, 80h/81h, `22h=A5h` giving `30h=A8h`, DMA TC,
equal paused counts, IRQ counts and the six OPL status values (00h, C0h,
C0h, E0h, A0h, 00h) agree with this model. Differences are DOSBox's: mixer
reset default 31 (F8h) instead of Creative's 24 (C0h), 82h without the 20h
bits in 0.74, DMA at 15 MiB valid there, and the last block of a D9h-
terminated 16-bit transfer raised as an 8-bit cause (with a reset instead of
D9h all eight IRQs report 02h); the model follows the Creative guide.

Open items for the lead: ADPCM decoding, E2h DMA identification and MPU-401
are not advertised; 45h/47h follow the later DSP 4.xx documentation and are
not in the guide revision above; timing granularity is the 1 ms host tick
(sub-millisecond boundaries are processed by timer-driven DMA); the
ledger `devices` count does not include the audio devices; the mailbox
address 0000:0600 is shared with VMDEV.COM by convention (separate VMs).
## f3-04

Kernel-owned DPMI 0.9 host (directive f3-04). Implementation:
`src/kernel/vm/dpmi.c` (discovery, entry, termination, services, V86 hooks),
`dpmi_desc.c` (LDT and descriptor checks, rows 0000h–000Dh), `dpmi_switch.c`
(transition records, 0300h–0306h, host-issued DOS calls), `dpmi_dispatch.c`
(protected-mode traps, interrupts, exceptions, CLI/STI/HLT/I/O emulation,
watchpoints); `ciuki/dpmi.h` is kernel-internal (no native ABI).

### Research (checked 2026-10-11)

- [DPMI 0.9 Committee specification](https://github.com/OpenRakis/Spice86/wiki/DPMI-0.9-Specs):
  INT 2Fh/1686h and 1687h, entry registers and initial selectors, the
  locked-stack rules for hardware interrupts and exceptions (sections 4 and
  10.3/10.4: the exception handler returns with a far return and "the original
  SS:(E)SP, CS:(E)IP and flags on the stack ... will be restored"), 0300h–0302h
  structures, 0303h callbacks (procedure entered with interrupts disabled,
  DS:(E)SI = real-mode stack, ES:(E)DI = structure, return by IRET), 0305h/
  0306h register conventions, virtual IF services 0900h–0902h.
- [DPMI 1.0 specification](https://sudleyplace.com/dpmione/dpmispec1.0.pdf)
  for the 1.0-only rows refused here and the 0.90 version encoding
  (DH=0, DL=90 decimal = 5Ah), and the
  [DJGPP DPMI index](https://www.delorie.com/djgpp/doc/dpmi/ch5.n.html) for
  numbering and per-function error outputs.
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  Vol. 3A: segment descriptors, LDT/LLDT, privilege checks on segment loads
  (ch. 3, 5); IDT gates, `INT n` through a DPL 0 gate raising #GP with error
  code `n*8+2` (IDT bit), error codes of #GP/#NP/#SS/#TS (ch. 6); debug
  registers DR0–DR3/DR6/DR7 and RF (ch. 17). Vol. 2: ENTER, IRET at CPL 3
  (IF and IOPL not restored when IOPL < CPL), LAR/LSL/VERR/VERW.
- DOS/4GW: `binw/dos4gw.doc` of Open Watcom 2.0 (the DOS16M switch mode is
  set automatically when DPMI is active); without HDPMI, VCPI or raw XMS
  access in a Ciuki VMM VM the extender can only run as a client of this
  host. Redistribution: [Watcom bundled the royalty-free DOS/4GW runtime
  for programs built with Watcom C/C++](https://en.wikipedia.org/wiki/DOS/4G);
  no redistribution text for the exact 1.97 binary was found in the
  Open Watcom 2.0 tree (open question, below).
- [libx86emu](https://github.com/wfeldt/libx86emu) 3.7 for the host runner;
  its `ENTER` sizes the frame-pointer push by the stack size instead of the
  operand size (`ops.c`, `x86emuOp_enter`), which corrupted DOS/4GW's 16-bit
  kernel on its 32-bit stack until the runner emulated ENTER itself.

### Decisions

- **Advertisement.** `DOSVM_DPMI_PUBLIC` (`ciuki/dpmi.h`) is 0: INT 2Fh/1687h
  is answered only in VMs created with `dosvm_config.dpmi` (the `f3:dpmi`
  probe). The lead sets it to 1 when `f3:dpmi` passes on qemu-t23, qemu-e500
  and qemu-min128. 1687h returns AX=0, BX=1, CL = CPU family (CPUID, 3–6),
  DX=005Ah, SI=0 (no host private area) and ES:DI=F000:1040h. 1686h returns
  AX=0 in protected mode.
- **Entry and clients.** The real-mode entry stub (INT FEh in the VM ROM,
  page F1000h) asks the personality for the PSP (host-issued INT 21h/62h),
  then switches: CS/DS/SS selectors for the caller's segments (64 KiB),
  ES = PSP selector (256 bytes), PSP:2Ch replaced by an environment selector
  and restored at termination. The first client fixes the width; an
  opposite-width nested entry fails with CF and changes nothing. Up to four
  nested clients per VM; each owns its selectors, memory blocks, DOS blocks,
  callbacks and watchpoints. Clients run at CPL 3 with IOPL 0 and EFLAGS.VM=0
  in the VM's address space; every port access is emulated against the VM's
  virtual devices.
- **LDT.** One 1024-entry LDT per VM (8 KiB of kernel memory), loaded through
  GDT index 8 (selector 40h) by the scheduler hook `dosvm_task_switch` when a
  DPMI worker is switched in, and unloaded otherwise. (Since 2026-10-25 the
  LDT descriptor is GDT index 10, selector 50h, and selector 40h is the VM's
  BIOS data area for its clients: execution-abi.md, "Selector 40h amendment".) Indices 0–15 are kept
  for 000Dh; client allocations start at 16; the host's code and locked-stack
  selectors and 32 callback stack selectors occupy the top. Only DPL 3 data
  or non-conforming code descriptors are accepted (no system descriptors,
  gates, conforming code or DPL < 3); base and limit are free, page
  permissions remain the boundary. Aliases from 0002h are VM-owned and freed
  when the last client exits.
- **Interrupt path.** INT n at CPL 3 reaches the kernel as #GP(n*8+2)
  because the IDT gates are DPL 0; INT3 and INT 80h use their DPL 3 gates
  and are delivered to the client's protected-mode vectors 3 and 80h; a
  DPMI client never reaches the native syscall path. `trap.c` routes every
  trap of a worker running a client frame (`dosvm_client`) to `dosvm_trap`;
  hardware IRQs are acknowledged by the kernel first.
- **Stacks.** Hardware interrupts, exceptions and callbacks run on a 16 KiB
  locked stack allocated from the VM's pool (segment-limit guarded, LIFO
  per nesting level). Real-mode reflections use a host stack at C600:2000
  (pages C6000h–C7FFFh made writable, outside the UMB arena and EMS frame),
  512 bytes per level. 32 transition records per VM; exhaustion fails the
  service with CF or, inside a fault path, terminates only the VM.
- **Virtual IF and CLI/STI.** The client's CLI/STI/PUSHF/POPF/IRET IF state
  is the monitor's virtual IF (0900h–0902h share it). The STI shadow is
  address based (the CS:EIP after STI), not TF based: a trap-flag shadow
  leaked TF into PUSHF images under DOS/4GW.
- **A20** is forced on while a client is in protected mode and the V86
  state is restored on every return to real mode.
- **Exception frames** are read at the current SS:ESP after the far return
  (or below the return address when the handler chains to the host default),
  not at the address the host pushed: DOS/4GW's error path moves the frame
  before returning. Interrupt and callback returns restore the interrupted
  context from the kernel record, so handlers may IRET from another stack;
  a return through the wrong host stub or without its record terminates the
  VM. Every frame returned to a client passes `dpmi_frame_check` (CS/SS/EIP
  valid for the IRET, unusable data selectors zeroed, IOPL 0, VM 0, IF 1).
- **Termination.** Protected-mode INT 21h/4Ch, 00h, 31h, INT 20h/27h and the
  V86 INT 21h/4Ch of the client's PSP reach one cleanup path: nested records
  unwound, the environment segment restored, memory blocks, DOS blocks,
  callbacks, watchpoints and selectors freed, real vectors restored, DPMI
  state reset when the last client exits. Faults unwind all nesting without
  guest help.
- **V86 monitor.** DOS VMs keep a virtual IOPL/NT image (`v86.h` `vflags`):
  PUSHF/POPF in V86 report what the guest last wrote; DOS/16M's CPU check
  ("protected mode available only with 386 or 486") needs IOPL bits to be
  writable as on a real 386 in real mode.

### Personality changes (CIUKIDOS, `src/boot/floppy_stage1.asm`)

- A fourth EXEC return-context slot (`saved_*4`, outside the 352-byte EXEC
  snapshot): kernel → SHELL.COM → COMMAND.COM → program → extender or child
  needs four nested frames; the fourth overwrote slot 3 before.
- INT 21h/48h with BX=0 grants one paragraph instead of failing: DOS/4GW
  allocates the host private area (1687h SI paragraphs) with INT 21h/48h
  even when SI=0 and stopped with "DPMI host error" on the refusal.
- The termination trampoline is chosen by the caller's CS against the COM
  load segment, so the host's protected-mode INT 21h/4Ch issues the real-mode
  call with the client's real-mode return address saved at entry.

### Payloads and probe

`scripts/build_dos_image.py` adds `\DPMI16.COM` and `\DPMI32.COM`
(`src/probes/dos/dpmi_client.asm`, BITS32=0/1), their nested children
`\DPMICH16.COM`/`\DPMICH32.COM`, the fault client `\DPMIFLT.COM`
(`dpmi_fault.asm`: LGDT inside a callback at depth 2), `\DPMI4GW.EXE`
(`src/probes/vm/dpmi_dos4gw.c`, `wcl386 -zq -bt=dos -l=dos4g`, Open Watcom
2.0 under `/opt/watcom`, deterministic with a fixed output name) and
`\SYSTEM\DOS4GW.EXE` (pinned SHA-256
`b8265123ac8a189637448618409ef3ecd2e9f3e1a47062c685a02240f688dec1`, found
through the CIUKIDOS PATH). The DOS manifest records them and a `dos4gw`
block (client, extender, hash, recipe, redistribution status). The 0.8
DOS/4GW probes beside it (`dpmi_ports.c`, `dpmi_lifetime.c`, `dpmi_fault.c`,
`dpmi_video.c`) report on COM1 and depend on the 0.8 HDPMI session adapters
in `src/vm/`; COM1 and the VGA registers are not virtual devices of the F3 VM
before f3-02. `dpmi_dos4gw.c` keeps their interrupt (protected-mode IRQ0 hook
with chaining), port (PIC) and lifetime (heap, DPMI blocks and selectors,
file I/O, normal exit) checks on the record port; the Mode-Y register checks
of `dpmi_ports.c` belong with f3-02.

Probe `f3:dpmi` (`src/kernel/probes/f3_probes_dpmi.c`, after `vm-devices`
in the F3 table and sweep): `group=dos_payload` hashes of the seven files
read from the VM's FAT16 disk (the runner compares them with
`dos.payloads` of the image manifest), then DPMI16 and DPMI32 each in a fresh
VM (`case=client` with verdict, entries and terminations, `case=counters`,
`case=rows` with `ok_all`, `refused`, `invalid` computed from the host's row
bitmaps, `case=release` after the exit), DPMI4GW (`case=client`,
`case=counters`, `case=dos4gw` with the extender path, version and
redistribution status, `case=release`), a VM without the flag where DPMI16 reports
`FAIL 01 01` (`case=gate`) and serves as witness while DPMIFLT faults in
another VM (`case=fault`), and `case=cleanup` against the ledger. Each
section destroys its own VMs and the later sections run after a failure,
so one QEMU run reports the whole row (the first error decides). Suite
`f3-vm.json`: `dpmi-qemu-t23`, `dpmi-qemu-e500`, `dpmi-qemu-min128`.

### Host verification (not QEMU evidence)

- `tests/host/dosvm_dpmi_test.inc` (in `dosvm_test`): descriptor and
  selector services, interrupts and translation, termination, callbacks and
  raw switches, memory/paging, DOS memory through a fake personality, IRQ
  and exception dispatch (including moved exception frames and wrong-stub
  returns), I/O and watchpoints, 16 nested levels and exhaustion, stale
  handlers and frames, 32-bit clients and nested cleanup, INT 1Ch/23h/24h
  reflection.
- `tests/host/dosvm_guest.c` + `dosvm_guest_cpu.c`: boots the real
  `DOSBOOT.IMG` in one VM with the production DOS VM, BIOS, XMS/EMS and DPMI
  code and libx86emu as the CPU, types the payload commands and reports
  their verdicts and the host state; run by `host_kernel_tests.sh` when
  libx86emu and an image with these payloads exist. On 2026-10-11 DPMI16
  with the VM's DPMI flag cleared reported `FAIL 01 01` (no host), DPMI16,
  DPMI32 and DPMI4GW passed and DPMIFLT stopped its VM with #GP at depth 2,
  every DPMI resource released after each exit. The runner found the
  personality and V86 defects listed above before any QEMU run.

### Limitations and open questions

- Mouse (INT 33h) callbacks are delivered only while the client is in real
  mode; protected-mode delivery is deferred to f3-06 workloads.
- Host page-fault resolution before the client's 0Eh handler has no
  producer yet; f3-02's VGA aperture will need it.
- CIUKIDOS terminations that do not pass through INT 21h/4Ch (internal
  aborts) are not intercepted; EXEC nesting deeper than four frames still
  exceeds the personality.
- DOS/4GW 1.97 redistribution text not located; the payload stays a local
  test client until the owner decides.
- Promotion: `DOSVM_DPMI_PUBLIC` stays 0 until the QEMU cases pass.

### First QEMU run (2026-10-12) and fixes

Run `d1a610eb` (qemu-t23, image `e0af9210…`): hashes passed, then
`DPMI16 FAIL 0D 02`. QEMU TCG does not check data-segment limits (recorded
with sources in `docs/validation/2026-10-09-f0/README.md`, f3-04 deviation);
the stage-13 read beyond a 255-byte limit completed. The clients now accept
that omission only under TCG's CPUID signature, print
`DPMIxx NOTE tcg_no_data_limit_check` and substitute a CLTS #GP(0); hardware
still requires the limit fault. The runner reproduces the failure with
`GUEST_TCG=1`, and `host_kernel_tests.sh` runs DPMI16/DPMI32 in that mode.
The same run stopped at the probe's screen dump: `\` and `>` are outside the
record grammar. The F3 controllers (`vm`, `dpmi`, `video`, `workloads`)
now share `probe_dos_screen` (`f3_probes_vm.c`, `ciuki/probe.h`), which maps
every glyph outside `[A-Za-z0-9_:.,/+-]` to `.`.

## f3-06

Reference DOS workloads and the F3 workload gate (directive f3-06; contract
row "F3 workloads"). Implementation and host evidence by Claude (Opus agent);
the QEMU `f3-workloads` cases, the owner-visible DOOM window and hardware
belong to the lead. No runtime PASS is claimed here.

### Research (checked 2026-10-11)

- DOOM, released source [id-Software/DOOM, linuxdoom-1.10](https://github.com/id-Software/DOOM):
  `g_game.c` `G_TimeDemo` sets `timingdemo` and `singletics` (one frame per
  game tic) and `G_CheckDemoStatus` ends a timed demo with
  `I_Error("timed %i gametics in %i realtics")`; `m_misc.c`
  `M_LoadDefaults` takes `-config <file>`. The released source has no DOS
  layer, so the DOS behaviour below is taken from the byte-matching
  reconstruction [gamesrc-ver-recreation-doom](https://github.com/fabiangreffrath/gamesrc-ver-recreation-doom)
  (GPL-2.0, `APPVER_DOOMREV` builds of 1.9): `i_ibm.c` `I_Error` ends with
  `exit(1)` after `I_Shutdown` returned to mode 03h, so the timedemo line is
  on the text screen and the exit code is 1; `I_InitGraphics` sets mode 13h
  then unchained Mode Y (SR04 chain-4 off, GC05/GC06 odd/even off, CRTC 14h
  dword off, 17h byte mode) and clears 64 KiB with map mask 0Fh;
  `I_UpdateBox` writes each plane with `outp(SC_INDEX+1, 1<<p)` and 16-bit
  stores; `I_FinishUpdate` flips with `outpw(CRTC_INDEX, CRTC_STARTHIGH+...)`
  (three pages A0000h/A4000h/A8000h); `I_BeginRead`/`I_EndRead` latch-copy
  the disk icon with write mode 1; `I_SetPalette` waits for vertical retrace
  on 3DAh; `I_StartupDPMI` locks the program (`_dpmi_lockregion`, 0600h);
  `I_ZoneBase` sizes the zone from 0500h, at most 8 MiB, at least 1.5 MiB;
  `i_sound.c` `I_StartupTimer` installs the DMX 140 Hz timer service even
  with `-nosound`, which sets both devices to `snd_none` (no SB or OPL
  detection); with devices 3 (Sound Blaster) the music device becomes the
  Adlib/OPL driver and `SB_Detect`/`AL_Detect` probe 220h/IRQ 7/DMA 1 and
  388h. Parameters and the frame-rate formula (gametics/realtics x 35) also
  appear in the DOOM FAQ v6.6, section 9-2 (shipped with the owner's copy as
  `DMFAQ66B.TXT`).
- The owner's `DOOM.EXE` (709 905 bytes, SHA-256 `799a20c7...`, v1.9) carries
  "DOS/4GW Professional Protected Mode Run-time ... 1.95" (Nov 23 1993):
  the extender is bound into the executable, no external `DOS4GW.EXE` is
  started (this fixes the "extender hash": it is part of `DOOM.EXE`'s hash).
  The [Open Watcom DOS/4GW guide](https://open-watcom.github.io/open-watcom-1.9/pguide.html)
  describes DOS/4GW running as a DPMI client when a host is present, the
  only mode a Ciuki VMM VM allows (no VCPI, no raw XMS switch).
- Wolfenstein 3-D, [id-Software/wolf3d WOLFSRC](https://github.com/id-Software/wolf3d):
  `ID_VL.C` `VL_DePlaneVGA` (mode 13h, then chain-4/odd-even off, write mode
  0, unchained 320x200); `ID_IN.C` `INL_KeyService` reads port 60h from its
  own INT 9 handler; `ID_MM.C` `MML_SetupXMS` takes UMBs through XMS
  function 10h; `WL_MAIN.C` `FinishSignon` prints "Press a key" and
  `IN_Ack`s, `DemoLoop` shows `NonShareware` (registered builds), `PG13`
  (7 s or a key) and the title (15 s or a key) before `US_ControlPanel`;
  `ID_US_1.C` accepts `NOWAIT`; `WL_MENU.C` starts the main menu on "Read
  This!" (`STARTITEM` without `GOODTIMES`), moves the cursor by initial letter
  (`HandleMenu`), `CP_NewGame` takes episode 1 and the default difficulty
  (`NewItems` index 2) with Enter, `CP_Quit` confirms with Y; `WL_PLAY.C`
  `CheckKeys` opens the quit dialog with F10.
- Mode X/Y register sequence: [Abrash, Graphics Programming Black Book, ch. 47](https://www.phatcode.net/res/224/files/html/ch47/47-02.html)
  (already the f3-02 reference).
- [Intel SDM Vol. 3A](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  4.7 (page-fault error code: P=1 protection violation, U/S=1 user mode,
  W/R) and 4.8 (the processor sets the PTE dirty bit on the first write
  through a translation).
- [Microsoft FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf),
  sections 3-6, for the fixture volumes; `dosfstools` `mkfs.fat`/`fsck.fat`
  and `mtools` as independent writer/checkers (already used by F1 fixtures).

### Decisions

**Workload disks.** CIUKIDOS serves one private volume, so a game cannot be a
second DOS drive. `dosvm_config.base` (new, `NULL` = `C:\SYSTEM\DOS\DOSBOOT.IMG`)
names another read-only base image of the same 128 MiB FAT16 geometry; the
writable overlay stays in `C:\SYSTEM\DOS`. `scripts/test/dos_fixtures.py`
builds that image per run: the DOS system read back from the canonical
build's `DOSBOOT.IMG` (manifest hashes, and the canonical image's
`dos.image_sha256` when the runner passes it), the game directory under
`\DOOM` or `\WOLF3D` (top-level plain 8.3 files; others skipped and listed)
and generated files (`\DOOM\CIUKI.CFG`: SB 220h/IRQ 7/DMA 1, sound and music
device 3, mouse enabled with click-to-capture in a window, no joystick).
It is embedded sparsely as `\CIUKIGAM.IMG`
(read-only attribute) in one contiguous chain of a 160 MiB FAT16 superfloppy
made by `mkfs.fat` (32 KiB clusters: the VFS walks at most 4096 FAT entries
per sector read); `fsck.fat` and `mtools` read both volumes back. The kernel
mounts the disk as a further drive; `dosvm_workload_base` (used by the
probes and by window VMs) looks for `X:\CIUKIGAM.IMG` (exact size, not a
directory) on drives D: to Z:. The VFS path is reused unchanged: no raw sector
access to a mounted volume, the VFS sharing mode keeps the base read-only.
The canonical image and `DOSBOOT.IMG` refuse payloads whose name or SHA-256
matches a local game file (`refuse_local_payloads`, files of 64 bytes or
more), and the generator writes only under `build/test-runs` or `build/host`.

**Launcher.** `\RUNREC.COM <dir> <program> [args]` (new, in `DOSBOOT.IMG`)
changes directory, EXECs the program with the arguments, reads INT 21h/4Dh
and reports `RUNREC PASS` (code 0) or `RUNREC FAIL Rcc Ttt` / `Ennnn` /
`Dnnnn` on the record port; the verdict latch keeps `Rcc_Ttt`. RUNREC is the
third and the game the fourth CIUKIDOS EXEC frame (DOOM's bound extender
EXECs nothing). `\CLILOOP.COM` (new) announces itself and spins in `cli; jmp
$`.

**Protected-mode VGA path.** `dpmi_pm_trap` resolved every client #PF to the
client. Now a fault with CR2 in A0000h-BFFFFh and error code P=1, U=1 (the
supervisor-only window, CR2 captured before interrupts) goes to
`dosvm_vga_fault_pm`: the same `cvx_execute` decoder and VGA bus as V86, with
`code32` from the code descriptor's D bit, CS's descriptor base, and a
`segment_base` callback resolving the data/stack selectors through the VM's
LDT on first use (an unusable selector is a bus fault: the VM stops with the
diagnostic instead of using base 0). REP strings restart after 4096
elements; DIV by a VRAM operand raises the client's exception 0. Linear
addresses come from the descriptors, so flat (DOOM, base 0) and 16-bit
clients with a selector over A0000h both work. V86 behaviour is unchanged
(the shared helper keeps the V86 64 KiB checks and the STI-shadow step).

**V86 single-step policy (monitor change).** With `track_boundaries`, the
f3-00 monitor returned to V86 code with TF set after every trap, so every
V86 instruction raised #DB. The libx86emu runner measured 687 M #DB traps in
700 M guest instructions while DOOM loaded its WAD through CIUKIDOS (and
840 M for a whole timedemo): far too slow for QEMU. `v86_prepare_return`
now sets TF only while a software STI/MOV-SS shadow is open (including a
return onto MOV SS/POP SS, as before) or an IRQ is pending for an enabled
guest; otherwise V86 code runs natively. Delivery points stay exact: every
instruction that can set the virtual IF (STI, POPF, IRET, INT n) traps at
IOPL 0 in V86 (Intel SDM Vol. 3, 8086 emulation, "Sensitive Instructions";
chapter 15 of the 245472-004 edition cited by f3-00), a host IRQ is a delivery point
after `dosvm_tick` raised the timer edges, and a trapped instruction inside
a MOV SS shadow is emulated before any delivery. Host shadow, PIC and
VMDEV-through-`dosvm_trap` tests are unchanged and pass; the runner's whole
DOOM timedemo fell from 840 M to 5.9 M traps with identical instruction
count, frames and result.

**POPF compatibility for DPMI clients** (closes the contract's open
question "Compatibility beyond 0.9" for the pinned DOOM build). On QEMU
(zz-doom5, 5fe5d14c) DOOM with sound stopped after `I_StartupTimer()`:
DMX saves its flags and disables (`pushfd; pop eax; cli` at 0024:7F15),
later restores them with `push eax; popfd`, then waits for timer ticks. At
CPL 3 and IOPL 0 the CLI traps but the POPF neither traps nor changes IF
(Intel SDM Vol. 2, POPF/IRET; DPMI 0.9 section 2.4: POPF and IRET "may not
modify the state of the interrupt flag"), so the virtual IF stayed clear,
IRQ0 stayed pending (IRR 01h) and the tick loop at 0024:83E2 never ended;
the libx86emu runner reproduced it exactly. The same loss later hung the
exit in `I_ShutdownSound` (`while (s != ticcount)` at 0024:E849, IRR 81h).
Design, IOPL unchanged, no timeout: the Windows 95 remedy described by
Raymond Chen ("Getting MS-DOS games to run on Windows 95: The interrupt
flag", The Old New Thing, 2016-04-11,
https://devblogs.microsoft.com/oldnewthing/20160411-00/?p=93281). When a
trapped CLI directly follows `PUSHF` (image on the stack top) or
`PUSHF; POP r` (image in r), the virtual IF was set, and the image equals
PUSHF's image of the current flags (IF set, IOPL 0, NT and TF clear,
status flags as now), the host sets TF in that saved image. The POPF or
IRET that later loads it sets TF; the single-step trap one instruction
later (Intel SDM Vol. 3, single-step exception condition) tells the host
the client restored "enabled": the host clears TF and sets the virtual IF;
any trap taken with TF set first applies that restore (a CLI right after
the POPF still disables). Images saved while virtually disabled (nested
sections) are not touched, so their restore keeps IF clear. Nothing is
single-stepped: the first design (stepping each section, 4096 instructions
at most) cost 42.6 M debug traps for one timedemo and still ran out on
DMX's mixing loops (10 128 sections hit the limit). Once a client holds an
armed image, a single-step trap without a watchpoint hit is the host's: a
debugger single-stepping that client is not supported. Counters
`if_armed` and `if_restored` in `dpmi_stats`; the doom `case=dpmi` record
reports `popf=` (restores). The libx86emu runner models TF loaded by
POPF/IRET (libx86emu loads it but never traps on it).

**Direct plane window.** One #PF per store is far too slow for Mode X/Y
games. When `cvga_direct_plane()` reports that a CPU write stores its byte
unchanged into exactly one plane (single-plane map mask, write mode 0, no
set/reset, rotate, logical operation, bit mask FFh, chain-4/odd-even off,
64 KiB window at A0000h), the window PTEs A0000h-AFFFFh map that plane of
the VM's own model (`cvga_state.plane`, now page-aligned in `struct dosvm`)
user-writable. Stores land in the model's memory, so the presenter, INT 10h
and the emulator need no copy; a plane switch (DOOM: four per frame; Wolf3D:
one per wall column) only rewrites 16 PTEs. Dirty bits of the mapped pages
are harvested into the presenter's granules when the window moves and before
each frame. Every misc/sequencer/graphics port write re-evaluates the state,
so a latch copy or any non-identity write mode closes the window before its
first access; B0000h-BFFFFh stays trapped; `dosvm_destroy` restores the VM's
own frames before `as_destroy`. Known deviation, recorded here: reads through
an open window return the mapped plane and do not load the latches (exact
while read map select equals the mapped plane, as in DOOM's and Wolf3D's
write-only blits). Counters: `dosvm_video_info` (faults, PM faults, CRTC
start-high writes = page flips, window openings, harvested pages).

**Probes and records** (`src/kernel/probes/f3_probes_workloads.c`, F3 table
and sweep after `dpmi`; selectors in the loader, kernel and runner models;
the runner's probe-name grammar now allows digits):

- `f3:dos4gw`: hashes `/DPMI4GW.EXE` and `/SYSTEM/DOS4GW.EXE` from the VM's
  disk (`group=dos_payload`, compared with the image manifest), records the
  extender (`case=extender ... version=1.97 redistribution=open_question`, as
  f3-04 left it), one warm-up launch, then 20 loops of fresh VM, `DPMI4GW.EXE`
  verdict, EXIT and destroy (`case=loop`): entries = terminations, no faults
  or stale handlers, DPMI resources released, and the VM ledger, free
  physical pages, task count and kernel heap equal the post-warm-up
  baseline (`case=baseline` diagnostic otherwise).
- `f3:doom` / `f3:doom-sound`: hash every file of `\DOOM` on the VM's disk
  (`group=workload_file`, `redistribution=local-only|free`, compared with
  the fixture manifest), `/RUNREC.COM` (`dos_payload`), the bound extender
  (`case=extender kind=bound_dos4gw_professional`); 5 loops of a fresh
  32 MiB DPMI VM booted from `CIUKIGAM.IMG` running `\RUNREC.COM \DOOM
  DOOM.EXE -config CIUKI.CFG [-nosound] -timedemo demo1`: `case=loop`
  (ms to the first DPMI entry and to the first page flip = launch time, run
  time, gametics/realtics parsed from the text screen latched at RUNREC's
  verdict line (`dosvm_verdict_text`: the CiukiOS shell redraws its row-0
  title bar over DOOM's line before the prompt; QEMU zz-doom c11506e9), RUNREC status, exit
  code 1 type 0), `case=video` (Mode Y 320x200 seen, flips, faults, PM
  faults, window openings), `case=dpmi` (`popf` = flags restores seen
  through armed images, informative), `case=audio` (f3-05 counters:
  DSP resets, block boundaries, PCM frames, IRQ edges, services, OPL events,
  DMA faults; `consumer` = the f4-05 native output attached on AC97
  profiles, not a pass condition), `case=loop_cleanup`. Without sound
  every audio counter must be 0; with sound DSP resets and OPL events >= 1.
- `f3:wolf3d`: real-mode VM (no DPMI flag) from `CIUKIGAM.IMG`; `RUNREC
  \WOLF3D WOLF3D.EXE`; the signon frame must stay unchanged for 6 s (its
  memory report precedes "Press a key"; all-black frames are ignored) and
  its presenter digest is recorded; Set-1 keys through the 8042 (focus):
  'N' at each further stable screen (2 s) until none appears for 12 s (the
  menu), Enter x3 (New Game, episode 1, default difficulty), >= 10 page
  flips in 3 s of play, F10 then Y; `case=loop` with XMS/EMS call counts and
  RUNREC exit code 0.
- `f3:vm-starve`: VM A (16 MiB, DPMI) runs `DPMI4GW.EXE` alone and then
  while VM B (8 MiB) spins in `CLILOOP.COM`; each run injects 8 Left Shift
  make/break pairs (16 IRQ1, no BIOS-buffer side effect) and measures IRQ0
  deliveries per ms and a native ring-3 heartbeat; `case=rates` requires
  timer >= 50 % and heartbeat >= 20 % of the reference, all 16 IRQ1 within
  1 s, B still running with zero IRQ0 deliveries (CLI held, host preemption
  intact). Thresholds are provisional until the first QEMU run.
- Game probes without the disk end `DATA case=fixture status=not_run
  reason=fixtures_absent`, `END status=NOT_RUN reason=fixtures_absent` (an
  accepted terminal for these three probes only); the runner itself records
  a case `not_run` with reason `fixtures_absent` before starting QEMU when
  the declared directory or a required file is missing.

Every record stays within the parser's 240 bytes and value grammar (paths
use `/`, drive letters alone); `tests/host/test_f3_workloads.py` checks the
worst cases.

**Suite** `tests/suites/f3-workloads.json`: `dos4gw-qemu-t23/min128`,
`doom-qemu-t23`, `doom-sound-qemu-t23`, `wolf3d-qemu-t23`,
`vm-starve-qemu-t23/min128`. Values only a first run can give are listed per
case in `pin_from_first_run` (marked `PIN-FROM-FIRST-RUN`): the sound
run's audio lower bounds, the Wolf3D signon/title digests and screen count,
the starvation thresholds. Until pinned, predicates check format and
structure (hex digests, >= bounds). DOOM's 1710 gametics (and flips >= 1710)
are already pinned from the host run below (`pinned_on_host`).

### Host verification (not QEMU evidence)

Worktree `wt/f3-workloads` on 2026-10-11 (base b5325f0 plus the merge
worktree's three compile fixes, applied identically):

- `make kernel`: PASS (clang -Werror, FPU/SIMD audit), VMM.ELF 2 921 716 bytes.
- `scripts/test/host_kernel_tests.sh`: PASS. DOS VM harness 1 611 918
  checks, largest record 212 bytes, including `dosvm_workloads_test.inc`:
  32-bit flat and 16-bit DPMI client stores and `REP STOSD` on the window
  through `dosvm_trap`, null selector stops the VM; direct window mapping,
  dirty-page harvest, closing on two-plane mask/write mode 1/bit mask/mode
  set, kernel reads and writes, presenter, restoration before
  `as_destroy` (the fake PMM refuses freeing a plane page); base image,
  workload lookup and window VMs on it. f3-02 goldens unchanged.
- `python3 scripts/build_dos_image.py`: T1 DOS PASS, 23 payloads (RUNREC,
  CLILOOP added), local-payload guard silent with the owner's directories
  present.
- `tests/host/test_f3_workloads.py` (8 tests): selectors and registration
  order, suite predicates against the probe's record formats with mutations,
  worst-case record lengths, runner not_run before QEMU, synthetic fixture
  (fsck, mtools read-back, sparse 13 MiB of 160 MiB, hash comparison and its
  failure modes, output refusals, payload guard).
- `python3 -m unittest discover -s tests/host`: 335 tests, 14 skipped, one
  error not caused by this directive: `test_dosvm` vm-boot fixture still has
  `devices_before=3` while `f3-vm.json` pins 4 since daff47e.
- libx86emu guest runner on this `DOSBOOT.IMG`: the f3-04 sequence PASS; 20
  consecutive `DPMI4GW.EXE` launches in one VM PASS with every DPMI resource
  released after each exit (475 M instructions).
- Real fixture from the owner's `third_party/Doom` (generated on the host, 0.6
  s, 26 files, 6 skipped, `DOOM.EXE` extender `dos4gw-professional-1.95-bound`)
  booted in the guest runner: DOS/4GW Professional 1.95 enters the kernel
  DPMI host, allocates 10.5 MiB of the pool, sets Mode Y 320x200 and renders
  and runs `-nosound -timedemo demo1` to completion: `timed 1710 gametics
  in 15190 realtics` on the text screen (realtics are the runner's simulated
  time), RUNREC `R01 T00` (exit code 1 from `I_Error`), 1714 page flips,
  2.31 M direct-window moves (map-mask writes, one per drawn column and
  plane), DPMI entries 1 = terminations 1, every DPMI resource released,
  2.59 G instructions and 5.9 M traps. 1710 gametics is pinned in the
  suite for this DOOM.EXE/DOOM.WAD pair. Wolf3D cannot run in this runner:
  its Borland start-up executes x87 loads the runner's CPU does not provide
  (QEMU does).

### Limitations and open items

- `DOSVM_DPMI_PUBLIC` is still 0: probe VMs pass `dosvm_config.dpmi`. For
  the owner-visible DOOM window without an ABI change, a `/Programs/dosbox` window
  VM boots an attached workload disk (`dosvm_workload_base`, `\CIUKIGAM.IMG`
  on D: to Z:) with the staged DPMI host; without that disk it keeps
  `DOSBOOT.IMG` and no DPMI host, as before. This is a lead decision to
  confirm (one line in `dosvm_client.c`); no QEMU case covers the window
  path yet. In the window: `\RUNREC.COM \DOOM DOOM.EXE` (or `CD \DOOM`, `DOOM`).
- DOOM/Wolf3D timing under TCG is unknown before the first run; the DOOM
  cases allow 7200 s for five timedemo loops.
- Wolf3D screen sequence depends on the build (NonShareware only in id's
  registered build); the script tolerates it, the pinned digests do not.
- Mode 13h chained stores (chain-4) still trap per instruction; only
  unchained single-plane stores use the direct window.
- DOS/4GW 1.97 (`DPMI4GW.EXE`'s extender) redistribution text is still not
  located (`redistribution=open_question`); DOOM's bound 1.95 extender is
  part of the owner's local payload.

### Amendment 2026-10-12: bundled shareware games

Owner decision 2026-10-12: Wolfenstein 3-D and DOOM are pre-installed games
of CiukiOS. Only their shareware releases are bundled; the registered data
(`third_party/WOLF3D`, `third_party/Doom`) stays local and keeps the rules
above. Implementation and host evidence by a Claude subagent (Opus) in
`wt/wolf-bundle`; the QEMU window evidence belongs to the lead. No runtime
PASS is claimed here.

#### Research (checked 2026-10-12)

- Packages and canonical sources. DOOM 1.9 shareware `doom19s.zip` from id's
  `idstuff` tree ([mirrors.lug.mtu.edu/idgames/idstuff/doom](https://mirrors.lug.mtu.edu/idgames/idstuff/doom/doom19s.txt),
  `ftp.fu-berlin.de/pc/msdos/games/idgames/idstuff/doom`). Wolfenstein 3-D 1.4
  shareware `1wolf14.zip` from the 3D Realms/Apogee `share` directory
  (mirrors `www.gamers.org/pub/mirrors/ftp.3drealms.com/share`,
  `ftp.zx.net.nz/pub/mirror/ftp.3drealms.com/share`, the mirror list of
  game-data-packager `etc/3drealms-mirrors`). Both mirrors of each package
  served byte-identical files.
- Published hashes. DOOM: SHA-256 `cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6`
  in the [AUR doom1-wad PKGBUILD](https://aur.archlinux.org/cgit/aur.git/plain/PKGBUILD?h=doom1-wad)
  and MD5 `244d181457c9be5f28b91b488e67e042` in [RPM Fusion doom-shareware `sources`](https://github.com/rpmfusion/doom-shareware/blob/master/sources).
  Wolfenstein 3-D: size 856,401, MD5 `a29432cd4a5184d552d8e5da8f80a531`, SHA-1
  `a4553d7ec4216061b9486c40be90b066862ea8e0`, SHA-256
  `cb2a2ef7ecef14152c65ff93cc3b84fbd3e8eb0c5c1de41a6fc8cdef559451a8` in
  [game-data-packager `data/wolf3d.yaml`](https://salsa.debian.org/games-team/game-data-packager/-/raw/master/data/wolf3d.yaml)
  (`1wolf14.zip?3drealms`) and the same MD5 in [RPM Fusion wolf3d-shareware `sources`](https://github.com/rpmfusion/wolf3d-shareware/blob/master/sources).
  Members: the eight WL1 files match game-data-packager's v1.4 shareware
  entries; DOOM.EXE MD5 `e2382b7d…` and DOOM1.WAD MD5 `f0cefca4…` match
  [doomwiki.org Doom files](https://doomwiki.org/wiki/Doom_files). The funet
  Apogee variant of `1wolf14.zip` (`c3d355ed…`, 728,992 bytes, `WOLF.1`
  inside) has one published reference only and carries a different
  `WOLF3D.EXE` build; it is not used.
- Containers. `doom19s.zip` holds `DOOMS_19.1` and `DOOMS_19.2`, one ZIP when
  concatenated (as RPM Fusion and AUR unpack it). `1wolf14.zip` holds
  `W3DSW14.SHR`, Apogee's installer container: layout from Hans de Goede's
  [extract.c](https://github.com/rpmfusion/wolf3d-shareware/blob/master/extract.c)
  (3Ah-byte header, 0A8h-byte entry headers, compressed size at +88h),
  members compressed with PKWARE DCL "implode"; the decoder is a port of
  Mark Adler's [blast.c](https://github.com/madler/zlib/blob/develop/contrib/blast/blast.c)
  (zlib licence), checked with the example in blast.c's header and by the
  member hashes above.
- Licence texts inside the packages (verbatim in each `UPSTREAM.json`,
  `license_clauses`). Wolfenstein 3-D, `VENDOR.DOC` [V.07.15.94] of Apogee
  Software, Ltd.: everyone may "copy, upload and generally pass around this
  Program without charging for it"; [3][A][1] "All of the Program's files,
  including this one, as released by us must be included without
  modification"; KEY POINTS and [3][C]: "If you wish to distribute in a
  retail location (such as on a rack), or as part of a hardware or software
  bundle, or on CD-ROM, you must get PRIOR signed written permission from
  Apogee, which is in Apogee's discretion and may be subject to royalty or
  other conditions." DOOM 1.9: the ENDOOM screen of `DOOM1.WAD` says "DOOM,
  Knee-Deep in the Dead can be freely distributed. Disk vendors should refer
  to the vendor information text file (vendor.doc) in your DOOM directory."
  (`doom19s.zip` contains no `VENDOR.DOC`); `DOOMS_19.DAT` "SHAREWARE
  VERSION" / "PLEASE DISTRIBUTE!!!"; `README.TXT` asks not to modify the
  shareware levels; John Carmack's 1999 e-mail quoted in Debian's
  [doom-wad-shareware copyright](https://tracker.debian.org/media/packages/d/doom-wad-shareware/copyright-1.9.fixed-4):
  "The DOOM shareware wad is freely distributable." The DOOM FAQ files
  (`DMFAQ66A`–`D.TXT`, by Hank Leukart) carry their own terms, II.(d): "the
  distributed form is not in an electronic magazine or within computer
  software (prior explicit permission may be obtained from Hank Leukart)".
- Source code. DOOM: GPL-2.0 ([id-Software/DOOM](https://github.com/id-Software/DOOM)).
  Wolfenstein 3-D: [id-Software/wolf3d](https://github.com/id-Software/wolf3d)
  carries id's 1995 LIMITED USE SOFTWARE LICENSE AGREEMENT (README.rst,
  LICENCE.DOC); John Carmack wrote that the code was later released under
  the GPL (`idstuff/wolf3d/wolf3d_iphone_v1.2_src.txt`). The bundled
  executables are id's original binaries, not rebuilt from source.

#### Decisions

**Vendoring.** `third_party/games/wolf3d-shareware/` and
`third_party/games/doom-shareware/` hold the unmodified archives, the
licence documents extracted byte-identical from them (`VENDOR.DOC`,
`FILE_ID.DIZ`; `README.TXT`, `DOOMS_19.DAT`) and `UPSTREAM.json` (sources,
hashes, references, licence clauses, `"redistribution": "free (shareware,
unmodified package)"`, a `bundling` note, every installed member's size and
SHA-256, excluded members with their reason). `.gitattributes` keeps the
bytes exact (`-text`).

**Games disk** (`scripts/build_games_disk.py`, called by
`scripts/build_image.py`; it never downloads). Each archive and member is
checked against its pins; an unpinned member that is not explicitly
excluded fails the build. `\WOLF3D` holds all 12 files of the release
(VENDOR.DOC [3][A][1]); `\DOOM` holds the 16 files of `DOOMS_19` except
the four FAQ files (their terms above), plus the f3-06 `CIUKI.CFG`.
DOOM's DOS/4GW Professional 1.95 is bound into `DOOM.EXE`; the package has
no separate `DOS4GW.EXE`. The disk is a DOS VM base of the `DOSBOOT.IMG`
geometry with the DOS system read back from the built `DOSBOOT.IMG`
(`dos_fixtures.dos_system`, manifest-checked), fsck'd and read back with
mtools; then its trailing all-zero sectors are cut: 7,262,208 of
134,217,728 bytes, so the 512 MiB system image gains about 7 MiB instead of
128 MiB. The manifest (`CIUKIGAM.json`) records every file (path, size,
SHA-256, role, package, `redistribution=free`), both the file and the full
logical image hashes, the DOS image hash and the licence documents. The
build is deterministic (`491c78c1…` twice).

**Guard.** `build_dos_image.refuse_local_payloads` now always refuses
registered data names (`DOOM.WAD`, `DOOM2.WAD`, `DOOMU.WAD`, `TNT.WAD`,
`PLUTONIA.WAD`, `*.WL6`, `*.SOD`, `*.SD1`–`3`, `*.N3D`) and accepts a file
that equals one of the owner's local files, or carries a reserved name,
only when its SHA-256 is pinned in a vendored shareware package
(`free=build_games_disk.free_hashes()`): DOOM 1.9 shareware and registered
share `SETUP.EXE`, the network tools and documents byte for byte. The
games builder applies it to the whole disk, and `build_image.py` to every
payload of the system image. `DOSBOOT.IMG` and the f3-06 fixtures are
unchanged.

**Image.** `/Games/CIUKIGAM.IMG` and `/system/licenses/<package>-<file>`
(licence documents and `UPSTREAM.json`); `build-manifest.json` gains a
`games` record (packages, files, sizes, hashes).

**Kernel** (`dosvm_disk.c`). `dosvm_window_base()` returns the owner's
local fixture disk (`X:\CIUKIGAM.IMG` on D:–Z:, exact geometry size, as
before) and, without one, the bundled `C:\GAMES\CIUKIGAM.IMG`; the window
client (`dosvm_client.c`) uses it, so every `/Programs/dosbox` window boots the
games disk with the staged DPMI host. `dosvm_workload_base()` (the f3-06
probes) keeps the fixture disk only: its pins are the owner's data. A base
may now be shorter than the geometry: a multiple of 512 bytes, at least
`DOSVM_BASE_MIN_SECTORS` (361, the boot, loader, FAT and root area), at most
the full 128 MiB; sectors past its end read as zeros without a VFS read,
writes go to the private overlay as before.

**Launcher.** `/Programs/dosbox [--name=TITLE] [COMMAND...]`: the command words,
joined by spaces, are typed at the DOS prompt through the window's own
input once the VM reports DOS ready (+400 ms) and the window has the focus
(`apps/dosbox/autorun.h`; no ABI change). A leading `\DIR\PROGRAM` becomes
`CD \DIR` and `PROGRAM [args]` (800 ms pause after each Enter) so the game
finds its data; US set-1 keys, Shift for capitals and `_ :`; one event per
10 ms loop (about 20 ms per character, slow enough for the shell to drain
the guest's 15-key BIOS buffer); a full guest queue retries, a focus loss resumes
without typing a character twice. Untypable or over-long commands make dosbox exit 2. The
Start menu's Programs submenu gets "Games" (when `/Programs/dosbox` exists) with
"Wolfenstein 3D (shareware)" (`\WOLF3D\WOLF3D.EXE`) and "DOOM (shareware)"
(`\DOOM\DOOM.EXE -config CIUKI.CFG`), Tango `applications-games`; each
launches `dosbox --name=<game> <command>` (`desk_task_launch_args`), and the
window's taskbar/title icon is `applications-games`. The Programs submenu
grows by one row after the program rows: `tests/host/desktop/shell_menu.py`
models it; with the submenu top-aligned under the top-bar Ciuki button
(wt/ui-polish) no program row moves, so `--regenerate` leaves the five
scripted desktop cases of `f2-desktop.json` unchanged (the scratch `zz-ui.json`
copy is no longer tracked).

**Licence status (owner).** DOOM's package terms permit free
redistribution and CiukiOS charges nothing. Wolfenstein 3-D's `VENDOR.DOC`
asks for Apogee's prior signed written permission for distribution "as part
of a hardware or software bundle, or on CD-ROM"; pre-installing it in the
image is such a bundle, and no permission is recorded. Vendoring the
unmodified package in the repository corresponds to the free on-line
distribution the same document allows. The owner decides before an image
with Wolfenstein 3-D is published (main prereleases are suspended); removing
it is one line (`PACKAGES` in `build_games_disk.py`, the `desk_games` row).

#### Host verification (not QEMU evidence)

- `make kernel`: PASS (FPU/SIMD audit passed); `make CAP= sdk`, `make CAP=
  desktop`: PASS (dosbox with autorun built for the target).
- `scripts/test/host_kernel_tests.sh`: PASS; dosvm harness 2,637,092 checks
  including the bundled-disk test (lookup order fixture > bundled, probes
  fixture-only, cut base zero fill without VFS reads, invalid sizes, window
  VM with DPMI on the bundled disk).
- `tests/host/test_games_disk.py` (12 tests): pins and references, member
  hashes against game-data-packager/doomwiki, VENDOR.DOC file list
  installed, verbatim clauses found in the documents, DCL decoder and SHR
  container, registered names and unpinned local files refused, a disk
  built beside fake local registered files holds none of them,
  determinism, cut image read back and extended to the logical hash.
- libx86emu guest runner (production DOS VM and DPMI host) on the cut
  `CIUKIGAM.IMG`, reading it at its real size: `\RUNREC.COM \DOOM DOOM.EXE
  -config CIUKI.CFG -nosound -timedemo demo1` → `timed 5026 gametics in
  43344 realtics` (realtics are the runner's simulated time), RUNREC
  `R01 T00` (exit code 1 from `I_Error`), 5,030 page flips, one DPMI client
  entered and terminated, 5.10 G instructions, guest runner PASS. Wolf3D
  cannot run there (x87, as above).

#### Lead QEMU checks

On the canonical image built from this state: from the desktop, Start >
Programs > Games > "Wolfenstein 3D (shareware)": a DOS window titled
"Wolfenstein 3D" types `CD \WOLF3D`, `WOLF3D.EXE` and the game reaches its
title and a playable episode 1; the same for "DOOM (shareware)" (DOOM
with SB16/OPL through `CIUKI.CFG`; keyboard only). Then `f3-vm` (the
vm-window case now runs on the games disk with the DPMI host),
`f3-workloads` (unchanged: fixture disk only), `f2-desktop` (the Programs
submenu now has a Games row) and the f2/f3 gates.

#### Open items

- Wolfenstein 3-D bundling permission (above): owner decision.
- No QEMU probe drives the bundled games yet (a `f3:doom` variant on the
  shareware disk would pin 5026 gametics); the f3-06 probes still end
  `NOT_RUN` without the owner's fixture disk.
- The typed command assumes the US keyboard decoding of the DOS VM and an
  idle shell prompt; a user typing during the 400 ms settle time mixes
  input.

## f3-07

Direct chained-mode window (directive f3-07). QEMU and hardware acceptance
belong to the lead; host evidence does not qualify either. Supersedes the
f3-06 limitation that chained mode-13h stores always trap per instruction.

### Research checked before implementation (2026-10-12)

- [FreeVGA sequencer registers](https://web.stanford.edu/class/cs140/projects/pintos/specs/freevga/vga/seqreg.htm):
  SR04 chain-4 selects the plane using address bits 0–1; SR02 still enables
  individual planes. Odd/even must be disabled and extended memory enabled.
- [FreeVGA graphics registers](https://web.stanford.edu/class/cs140/projects/pintos/specs/freevga/vga/graphreg.htm)
  and [display-memory access](https://web.stanford.edu/class/cs140/projects/pintos/specs/freevga/vga/vgamem.htm):
  write mode 0 is a plain byte store only with enable-set/reset clear,
  rotate/logical operation zero and bit mask FFh. Read mode 1 and odd/even
  reads cannot use a linear mapping. Hardware reads load all four latches;
  those latches do not affect identity write-mode-0 stores.
- [DOSBox Staging upstream `vga_memory.cpp`](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/video/vga_memory.cpp):
  chained handlers separate CPU byte addressing from plane storage and
  scanout caches. Consulted for behavior only; no upstream code copied.
  The repository's `memory_address`, `read_vram`, `cvga_write_vram` and
  scanout use physical plane offsets: byte `o` selects plane `o & 3` at
  `o & FFFCh`, rather than a compacted `o >> 2`. That existing layout is
  retained, including the untouched intervening plane bytes.

### Decisions

- Reuse the VM's sixteen already allocated A0000h–AFFFFh backing pages as
  the linear 64 KiB buffer, seeded from the four planes on opening. No new
  allocation, commitment, residency or memory limit is required. These
  pages may be physically noncontiguous; the shared V86/DPMI page table
  maps each one user-writable. B0000h–BFFFFh remains trapped. The existing
  single-plane mapping and its counters retain their behavior.
- Enable only the standard chained 320x200 graphics state: SR04 bits 1–3
  set, SR02=0Fh, A0000h 64 KiB aperture, read/write mode 0, GC01/03 zero,
  GC08=FFh, 256-color shift, and mode-13h CRTC addressing. Palette and
  start-address changes remain trapped and counted.
- Harvest CPU-dirty linear pages using the existing PTE dirty-bit/TLB
  machinery. Deinterleave only those pages into the model using
  `cvga_store_plane`; dirty presenter granules are set for changed bytes.
  Close before BIOS/VBE calls, register changes that affect access or CRTC
  mode, model reads/writes, presentation, VM stop and low-page restoration.
  Palette/start-address writes do not need to close the linear mapping.
  Re-evaluate after port/BIOS changes; an eligible aperture protection
  fault after closure opens the mapping and retries the original instruction,
  with no EIP/register advancement or V86 burst. Retry faults are counted.
- Direct reads return the same bytes as trapped read mode 0, independent of
  GC04. As for the existing plane window, direct reads do not load latches:
  no load is needed for identity chain-4 write-mode-0 stores. Enabling a
  latch-dependent operation closes the mapping before subsequent reads.
  A program which reads while mapped, switches to write mode 1 and consumes
  that earlier read's latches without another read retains the f3-06
  limitation; this optimization does not claim to track native CPU reads.
- `linear_maps` counts openings and `linear_harvests` counts dirty linear
  pages harvested; plane `maps`/`flushes` remain separate. The DOOM
  `case=video` row gains both counters; `faults`/`flushes` move to a
  companion `case=video_detail` row so the 240-byte record envelope holds
  even with maximum 32-bit counters. Existing suite fields stay available.

### Host verification (worktree `wt/f3-07`, 2026-10-12)

- `make kernel`: PASS, clang i686 `-Werror`, FPU/SIMD audit PASS;
  `build/f0/VMM.ELF` 3,471,736 bytes, base `af6369cd5a34` with local changes.
- `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`: PASS.
  The initial unmodified environment failed at LeakSanitizer because this
  sandbox denies its process inspection; ASan and UBSan remain enabled.
  The pre-existing Wolf3D replay was missing its closing `}` in the base;
  that compile blocker was corrected without changing the replay.
- DOS VM harness: **4,811,002 checks**, PASS. V86 replay through
  `dosvm_trap`: 32,768 stores, 32,768 trapped faults versus **1** with the
  linear window. Flat-selector PM replay through `dosvm_trap` /
  `dpmi_pm_trap`: 16,384 stores, 16,384 trapped faults versus **1** with the
  window. That one fault retries after a deliberate mid-frame closure.
  The no-window replay disables only GC06 graphics scanout while preserving
  identical chain-4 memory cycles, restoring graphics before presentation.
  Both compare every plane byte (including sentinel holes), every mapped
  read for all four GC04 selections and the unused end of the 64 KiB window.
  Closure tests cover the directive's indexed port writes (both selected
  CRTC pairs), set/reset, odd/even, aperture, BIOS clear/preserve, VBE,
  kernel peek/store, latch copy, presenter and VM stop/destruction. Clean
  pages are not harvested twice; granules represent only changed bytes.
- All five f3-02 goldens unchanged, including a mode-13h pattern written
  through the actual linear backing/PTE dirty-bit seam (stage 3) and the
  stage-4 Mode X golden. Wolf3D replays unchanged: 76 / 152 / 0 faults,
  0 / 0 / 304 plane maps, one flip each. Independent VGA vectors:
  173,117 assertions PASS; `cvga_state` remains 267,092 bytes.
- Standard libx86emu guest probes: PASS (27,156,373 instructions,
  212,345 traps, 0 unexpected); TCG-model guest probes PASS (853,698
  instructions, 174,763 traps, 0 unexpected).
- `python3 scripts/build_dos_image.py`: T1 DOS PASS, 24 payloads read back,
  SHA-256 `0a48c8e347c4211033443ca7d9dab2d34ef3362be29732ee2eb97d95f58d3e62`.
  DOOM fixture fsck/mtools read-back PASS; supplemental libx86emu run of
  `-nosound -timedemo demo1`: **timed 1710 gametics in 15162 realtics**,
  **1714 flips**, RUNREC `R01_T00` (the expected timedemo exit 1), DPMI
  resources released, guest runner PASS (2,584,560,273 instructions,
  5,189,202 traps, 0 unexpected). The runner writes the model directly:
  its zero `pm_faults` is not evidence for the mapped window or QEMU timing.
- Focused Python tests (`test_f3_workloads`, `test_dosvm`, `test_vga_x86`):
  19 tests PASS. Record-envelope tests retain the existing suite fields
  and cover maximum counters plus maximum CRTC geometry (231-byte video
  row for `doom-sound`). `git diff --check`: PASS.

### Lead gate and limitations

Build the canonical image once after review/integration, then qualify
`doom-qemu-t23` (five loops: gametics=1710, flips approximately 1714,
pm_faults <= 1000 per loop, at least 100x below the previous result).
Inspect `linear_maps`/`linear_harvests` and the companion video-detail rows;
retain the vm-video and vm-window digests and wolf3d-qemu-t23 evidence.
The existing workload suite still has its previous PM-fault lower-bound
predicate; tightening the performance gate belongs to the lead's QEMU
qualification. Run the earlier integrated F0/F1/F2/F3 gates and complete
the T23 DOOM hardware run. No QEMU, full image/CD build, hardware test,
commit or push was performed here. The complete verbatim host outputs and
handoff are in `build/codex/f3-07-report.md`.

## f3-13: command loading and DOS window presentation (2026-10-13/14)

Directive: `docs/directives/f3-13-dosbox-loading-and-speed.md`. Research,
decisions and limitations: `docs/validation/2026-10-13-f3-13-host.md`.
The version-1, 136-byte syscall amendment is in `execution-abi.md`.

Command launches show a themed loading panel with the catalogue icon and an
indeterminate progress pulse. The guest's own `CLS` removes boot text before
the directory/program command. An owner-armed EXEC/video boundary releases
presentation; text output in the same mode is supported. Startup failures
retain an error panel. The plain prompt uses its existing visible boot path.

This amendment supersedes f3-07's requirement to close the chain-4 window on
each window presentation. Dirty bits are harvested and rearmed while the
writable mapping stays open. Model/digest consumers, latch/mask/mode changes
and teardown retain their explicit closure rules and physical plane layout.
Conversion uses indexed rows and an XRGB lookup; source damage groups span
eight lines, and the native client submits only changed destination band runs.
Completion writes can present immediately; unmarked linear frames use the
programmed VGA refresh as a fallback. No 33 ms presentation cap remains.

Host acceptance: kernel suites, five VGA goldens, the native launch replay,
f3-workloads host controls and before/after production-presenter benchmark.
Commands/results are in `build/codex/f3-13-report.md`. Integration still
requires the lead's `f3-workloads` QEMU suite, earlier gates, and T23 evidence
for the requested DOOM 35 FPS and Wolf3D 70 FPS. Host throughput and completion
counters alone do not qualify those hardware targets.

## f3-19 — windowed VGA port swaps, Mode X/Y rows and PM bursts

Directive: `docs/directives/f3-19-dos-window-video-fast-paths.md`; research:
[`research/dos-vm-video-2026-10-14.md`](research/dos-vm-video-2026-10-14.md).
This amendment supersedes the four-plane-copy cost premise and the planar
CRTC-harvest rule in f3-06/f3-16. DOOM draws columns and floor spans directly
in VRAM; the replay uses 1,279 map-mask OUTs per frame and three page bases.
Full-screen hardware ownership remains outside this implementation.

Sources checked against the implementation:

- [Intel 80386 paging reference, sections 5.2.4–5.2.5](https://www.scs.stanford.edu/05au-cs240c/lab/i386/s05_02.htm):
  the CPU sets A/D; clearing D or changing a translation needs invalidation.
  `as_replace_pages` replaces up to 32 existing entries in one page table,
  returns their old A/D masks, and reloads the active CR3 once. It allocates
  nothing, changes no ownership counters, validates the whole span before
  mutation, and skips a flush for clean unchanged/inactive mappings. Callers
  exclude guest execution and interrupts. The selected operation is one
  CR3 reload; comparing its real cost with 16 INVLPGs remains a T23 task.
- [FreeVGA sequencer](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/seqreg.htm)
  and [graphics controller](https://www.scs.stanford.edu/22wi-cs212/pintos/specs/freevga/vga/graphreg.htm):
  SR02 selects write planes; GC04 selects the read plane independently.
  Shared port callbacks fast-path SR02/GC04/GC05 in both V86 and PM, including
  low-byte-first indexed OUTW. A swap records the old plane's D-page mask in
  `video_pending`; it does not harvest granules or use `as_map`/`as_lookup`.
  The presenter merges those masks once, including when no window remains
  open. Planar CRTC 0C/0D writes select the source without a harvest. Chain-4
  still publishes its separate linear backing at completion boundaries.
- [Intel MOVS reference](https://www.scs.stanford.edu/05au-cs240c/lab/i386/MOVS.htm)
  and [Wolf3D drawing source](https://raw.githubusercontent.com/id-Software/wolf3d/master/WOLFSRC/WL_DRAW.C):
  bounded planar REP MOVS/STOS runs preserve element order, source-before-
  destination reads, DF, address/operand sizes and 16-bit register high
  halves. RAM source pages may be fragmented. Identity mode-0 fills/copies
  and VGA-to-VGA mode-1 MOVSB have bulk paths; other modes remain bytewise.
  Mode-1 MOVSW/MOVSD intentionally fall back: their last source-byte latch
  would be written to every destination byte, unlike a plane memmove.

Mode X/Y scanout caches row addresses and panning through the existing
`setup_row` rules (split, preset, repeat and 64 KiB wrap). A guarded byte-
addressed, unchained indexed converter fuses four-plane reads, DAC-mask/LUT
lookup and scaling into the XRGB row, avoiding an intermediate index row.
Other scanout formats keep the generic converter. Only dirty source rows
are converted; unchanged black borders survive page flips. Existing
8-line destination damage transport and the five VMVIDEO digests remain.
Windowed graphics sample on a flip or the programmed refresh, including
multi-plane trapped drawing with no writable mapping.

PM aperture faults can continue through the same preflight/commit burst as
V86: up to 1,024 instructions, stopping after eight consecutive instructions
without VGA memory/ports (enough for INC/INC/LOOP and the following RAM load).
Register ALU/setup, short Jcc/JMP/LOOP and VGA scalar IN/OUT connect planar
runs. All PM fetches/operands validate descriptor rights and limits. Stores
outside VGA, unsupported instructions, TF/watchpoints, virtual-IF recovery,
return tracking, STI/MOV-SS shadows, deliverable IRQs and rescheduling stop
the burst before the continuation. A partial generic REP retains its
restart point; a fully validated planar run of at most 64 KiB completes in
one exit. No DPMI dispatcher, V86 decoder or scheduling policy is changed.

The f3-16 direct-read limitation remains accepted for these write-only draw
runs: a native read of a writable plane mapping returns that mapped plane
and does not load the VGA latches. An explicit GC04 write now selects its
read shadow, read-only when it differs from the write plane; a subsequent
SR02 write resumes the write window, even with the same mask. This fixes
explicit read-map sequences such as DOOM fuzz columns, but does not create
independent native read/write translations: arbitrary reads after an SR02
write with a different GC04 still have the pre-existing mapped-read
limitation. Latch-dependent modes close the window and use the full model;
mode-1 blits load all four source latches before writing. A native read
followed by a latch-dependent store without another trapped read still
cannot recover the earlier latches. `latch_read_windows` conservatively
counts A-without-D pages when a window closes; it is not proof of a bad blit.

Verbose counters add per-register write counts/handler time and whole-exit
costs (the final VGA register of a coalesced exit), port remaps/remap time
and burst instructions. `vm_beat_vga` keeps the original `faults` record and
adds `part=fast` (`port_remaps`, `burst_insn`) and `part=time` (`remap_us`,
`present_us`) companion records to stay within 240 bytes. Per-second
samples are retained in memory and dumped after stopping, as before.
`vm_perf_wall` exposes monitor permille and wall time off the VM CPU;
remap/harvest are subsets of monitor/presenter work, not additive shares.
Off-CPU time includes native presentation/composition, other tasks and idle;
this scope does not distinguish those by changing scheduling/compositor
accounting. Host replay timings are not P3/T23 TSC or achieved game fps.

Host evidence and the remaining QEMU/T23 gates are recorded in
[`../validation/2026-10-14-f3-19-host.md`](../validation/2026-10-14-f3-19-host.md).

## fx-27 — windowed DOS graphics: lean port exits, batched palette loads, lazy chain-4 planes, dirty-row presentation

Directive: `docs/directives/t23-f441c5bd-trial-fixes.md` (fx-27). Written
2026-10-23 23:22 before the code changes it describes were finalised;
measurements and commands: [`../validation/fx-27-windowed-dos-graphics.md`](../validation/fx-27-windowed-dos-graphics.md).
This amendment supersedes, for windowed VMs, f3-19's "the selected
operation is one CR3 reload", f3-13's per-presentation chain-4 publication
(the linear backing is deinterleaved only for a model consumer) and the
f3-41 VBE rule that any dirty page redraws every row.

### Evidence (T23 trials)

- f441c5bd, Quake in a window (BOOT.LOG banks n=3/n=4, 5 s each): 31 and 11
  frames; `vga_io` 17,677 exits / 386 ms, all `03c9` (DAC data, 21.8 us
  each: about 570 per frame, 12.4 ms per frame); `pit_pic` 12,909 / 293 ms
  and 26,536 / 666 ms (`0040`/`0043`, Quake's `Sys_FloatTime` latch and two
  reads, 22.7 us each); DMA position polls `00c6`/`00d8`; present 5.6 ms and
  harvest 1.05 ms per frame (16 pages, 291 rows); monitor 42 %, off-CPU 42 %.
- e831e685 and a79f840c, DOOM in a window: 14–20 us per VGA port exit,
  about 950–1300 map-mask OUTs per frame (planar.asm one per column, one per
  plane per span); the fx-16 T23 model attributes 44 % of the CPU to them.

### Reference behaviour

- Windows 3.0/95 VDD: "When an application is running in the background,
  the VDD traps all the video I/O, saving the output port values and
  emulating the input port values" (Windows 3.0 DDK *Virtual Device
  Adaptation Guide*, 18.1.2); the grabber repaints incrementally: "Get_Mod
  returns a data structure which indicates modifications to the current
  display. The Grabber DLL will modify only those parts of the window that
  have changed and then issue a Clear_Mod" (18.4.1); VMM's ModifyPageBits
  "always has the side effect of clearing P_DIRTY and P_ACC" (chapter 19):
  page-table dirty bits are the modification record. A windowed VM's port
  writes are therefore trapped by design; CiukiOS keeps that model and makes
  each trapped write cheap, and repaints only what changed.
- DOSBox 0.74 (`vga_memory.cpp` `VGA_ChainedVGA_Handler`): chained mode-13h
  stores also go to a linear `fastmem` copy that the renderer draws from;
  `render.cpp` compares every scanline with a cache of the last frame and
  passes only changed lines to `GFX_EndUpdate`; `RENDER_SetPal` accumulates
  palette entries (first/last) and `Check_Palette` applies them once at the
  next frame, which is then a full update.
- QEMU 10.1 `hw/display/vga.c` `vga_draw_graphic`: per-scanline dirty-page
  test on a memory snapshot, a changed palette (`update_palette256`) forces
  a full update, `dpy_gfx_update` per run of changed lines.
- Intel SDM Vol. 3A 4.10.4.1: MOV to CR3 invalidates every non-global TLB
  entry; CiukiOS maps the kernel without PTE_G. Linux `arch/x86/mm/tlb.c`
  invalidates ranges of up to `tlb_single_page_flush_ceiling` = 33 pages
  with INVLPG ("balancing the cycles you spend doing invlpg with the cycles
  that you spend refilling the TLB later", `Documentation/arch/x86/tlb.rst`).
- Agner Fog, *Instruction tables* (Pentium M, Pentium II/III): CPUID about
  130 cycles, RDTSC about 42: the monitor's serialized clock read is about
  0.27 us on the T23, a small part of a 14–22 us exit.
- Quake 1.06 (QUAKE.EXE disassembled): `VGA_SetPalette` per byte
  `movzx eax,[edi+esi]; mov ecx,ebx; sar eax,cl; mov edx,3C9h; out dx,al;
  inc edi; cmp edi,2FFh; jng`; `Sys_FloatTime` calls `dos_outportb(43h,0)`
  and `dos_inportb(40h)` twice. DOOM 1.9 `i_ibm.c` `I_SetPalette`: 768
  OUTs to 3C9h after one to 3C8h.

### Behaviour

1. **Lean port exits.** A #GP(0) on IN/OUT (optionally 66h-prefixed) to a
   hot emulated port — the VGA register file (3C0h–3CFh, 3D4h/3D5h/3DAh and
   the monochrome 3B4h/3B5h/3BAh, byte or word), the PIT (40h–43h), the PIC
   (20h/21h/A0h/A1h), AdLib/OPL (388h–38Bh) and the DMA controllers (00h–0Fh,
   80h–8Fh, C0h–DFh), byte only — is emulated by the same handler and the
   same I/O bursts as on the ordinary path (entered only when the next
   opcode passes the burst's own first test or a DAC write is open), then
   returns without the ordinary path's per-exit housekeeping and with one
   serialized clock read fewer in its cost sample, when before the instruction the
   VM is booted and running (windowed or full screen), has no full-screen hold, idle poll,
   IPX/NE2000 binding, pending SB16 IRQ-entry stamp, STI or MOV SS shadow,
   single-step, mouse callback due, and, with a DPMI client, no
   IF recovery, return tracker, client watchpoint or CLI-seed demand
   (fx-16), is not a host stub and is not the saved return point of an open
   hardware-interrupt record; and when after it (IF clear until IRET) no
   virtual IRQ is pending, no reschedule is due, no DMA channel waits to be
   pinned (f3-46 settle point), the frame is not at a launch/EXEC return and
   not at an interrupted context's return point. Otherwise the exit
   continues through the ordinary tail without emulating again, so IRQ
   delivery, IF recovery, scheduling and pinning happen at the same
   boundary as before. The skipped work (UART/IPX service, present
   sampling, heartbeat/stall diagnostics, reports, audio log, file-service
   watchdog) is periodic; the host timer's own exit always takes the
   ordinary path, so it runs at most one host tick later. A lean PM exit
   keeps the DPMI bookkeeping the ordinary path leaves (decode seed at the
   returned boundary, skipped-seed count unless a flags image is armed,
   `if_recovery_end`). An armed fx-16 flags image (PUSHF image patched with
   TF at its CLI, not yet loaded by its POPF) does not refuse a lean exit:
   at an I/O exit without TF and without a pending IRQ the ordinary path
   does nothing for it (`flags_restored` needs the step, the boundary seed
   returns first, the I/O and DAC bursts refuse it themselves), and DOOM
   1.9 keeps such an image armed across most of its frame (section 6 of the
   validation record).
2. **PTE batches invalidate per entry.** `as_replace_pages` (plane swaps,
   D-bit collection) issues INVLPG for each changed entry (at most 32) of
   the active address space and never reloads CR3.
3. **Palette loads are one exit.** After a trapped DAC write (3C8h/3C9h)
   the monitor looks ahead at most 8 instructions, on a copy, for the next
   DAC (3C6h–3C9h) or status-register (3DAh/3BAh read) access. Only
   instructions without memory stores or VGA memory reads qualify (register
   setup/ALU/shifts, short branches, loads from resident user pages; PM
   descriptors are checked as `dpmi_ptr` does, once per burst); when found,
   the copy's state is committed and the port instruction executes through
   the VGA port callback. The burst stops on anything else, on TF,
   watchpoints, IF-image/return tracking, shadows, a deliverable IRQ, a
   reschedule, a host tick and after 8192 instructions. A whole palette
   load is presented with its final values (one display change).
4. **Chain-4 planes are deinterleaved lazily.** While the linear window is
   open its 16 backing pages are the chained image. A collection (present,
   present sample, CRTC flip) records D pages as stale and as presenter
   damage; `dosvm_vga_model`/`dosvm_vga_sync`, a window switch and the
   presenter's planar path deinterleave the stale pages first, so every
   consumer of the planes still sees exact planes.
5. **Chained rows come from the linear backing.** With every row base
   dword-aligned, dot x of row y is linear byte `(base + pan + x) & FFFFh`;
   rows touching damaged pages are compared byte-exactly with the last
   displayed indexes and only differing rows are converted (LUT, 2x) and
   written. Any other scanout uses the planes (unchanged f3-19 path).
6. **Proven rows are written without read-back.** A destination row whose
   displayed indexes differ is written to the shared surface directly; rows
   redrawn only because of a palette, geometry, blank or layout change are
   still compared with the surface first (unchanged rows keep no damage).
7. **VBE rows from dirty pages.** The windowed VBE presenter collects and
   clears the D bits of the visible buffer pages and converts only rows
   whose source lies in a dirty page; a palette or mode change redraws all.
   Columns step with an exact DDA (no division per pixel), 8-bit pixels use
   the shared 256-entry palette LUT, 640-wide 8-bit rows convert directly.
8. **Records.** `vm_perf_remap` adds `lean=` (exits retired lean), `dac=`
   (OUTs retired by palette bursts) and `linear=` (rows converted from the
   linear backing) to the five-second bank.

### Kept, and limits

- Every map-mask/read-map change still traps (a different plane shadow
  backs the same addresses; Windows traps it too). DOOM's ~1,000 OUTs per
  frame become ~1,000 lean exits.
- The DOS window's surface stays 640x480 XRGB (no ABI change). The desktop
  composes it and presents duplicated scaled rows with 2D-engine screen
  copies (f2-92 `present_box_raw`); that path is unchanged here.
- fx-doomfs2 amendment: non-VGA hot ports and PM CLI/STI use the fx-27
  lean exit in every full screen. VGA register ports qualify only while
  modeled (window or VBE full screen); native VGA full screen keeps direct
  adapter I/O, and any trapped VGA access there takes the ordinary exit.
  Mouse callbacks due also refuse lean exits in protected mode.
- The SB16 DSP ports (22xh), the keyboard (60h/64h) and the game port
  (201h) keep the ordinary exit; so do exits during a
  CLI-seed demand window (fx-16) and every exit with a pending IRQ.
