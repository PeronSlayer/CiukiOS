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

Retrace: the presenter's frame clock is 33 host ticks; the last three are
vertical retrace (status bits 3 and 0). In the active period bit 0
alternates on successive reads as a horizontal-blanking stand-in, so
snow-avoiding loops progress; polling never advances the frame clock.

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
exact `/bin/dosbox` whose parent is the supervisor or owns both a display
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

`/bin/dosbox` (apps/dosbox, MIT): one process, one VM, one window titled
with the VM name (`--name=`, default `CiukiDOS`), protocol v2 close request
→ CLOSE and exit, VM stop → window close, focus/key/motion/button forwarding,
damage at the kernel cadence. The desktop passes `--channel-fd=3` to it and
gains `--launch=<program>`, which starts one `/bin` menu program through the
menu-click path (used by `f3:vm-window`). Adding `/bin/dosbox` grows the
bottom-anchored Ciuki menu by one row, so the f2-30 `desktop-shell-*`
scripted click on Demo moves from `y-169` to `y-197` (1024x768).

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
`/bin/desktop --launch=dosbox` under a controller owner, waits for the bound
window VM and its prompt (`case=window`), emits `ARM action=window_input`
(the runner moves the pointer off the window and types `\VMVIDEO.COM` with
QMP keys; without that stimulus for 30 s, as in a hardware cfg sweep, the
controller types it and records `typed=kernel`, which the QEMU cases
refuse), hashes stage 1 (`case=frame stage=1 typed=window`), emits `ARM
action=window_screen digest=` and holds the frame for 6 s. The runner's new
`window_screen` check takes a QMP screendump 2 s after that ARM and requires
the composited client region (origin 23,76: first window at 20,52 plus the
3-pixel border and 24-pixel title) to hash, as XRGB rows, to both the
suite's stage-1 golden and the ARM digest. The probe then finishes the
stages and types EXIT: `case=closed` requires reason 0, the window binding
released and `/bin/dosbox` exited; `case=cleanup` requires the VM ledger and
the desktop object ledger (descriptions, surfaces, pages, channels,
messages, grants) back at baseline. Cases: `vm-window-qemu-desktop-2002`
and `vm-window-qemu-desktop-1998` (needs a 24/32-bit desktop of at least
663x556).

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
