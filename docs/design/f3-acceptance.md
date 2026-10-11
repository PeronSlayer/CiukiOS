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
rule: at a boundary the DSP closes the block's segment with its cause,
latches the cause, raises the edge if the line was low and stops consuming
until the guest acknowledges that cause, or until the next block is due on
the nominal timeline and at least one host tick has passed; that resumption
is counted as `late`, its segment is flagged `LATE`, and a block started
after two unserviced boundaries is flagged `STALE` (counted). An
acknowledgement immediately consumes the part of the next block already
due, so a responsive guest services sub-millisecond blocks without lag and a
backlog drains one service per block. Each call processes at most 64 ms of
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
(sub-millisecond blocks rely on acknowledgement-driven catch-up); the
ledger `devices` count does not include the audio devices; the mailbox
address 0000:0600 is shared with VMDEV.COM by convention (separate VMs).
