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
  DPMI worker is switched in, and unloaded otherwise. Indices 0–15 are kept
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
