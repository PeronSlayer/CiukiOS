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

Fault diagnostics added to the controller records: `group=vm_stop` carries
`address` (CR2 read before interrupts are re-enabled), `error` (page-fault
error code) and `code` (the eight bytes at the faulting CS:IP); a stopped VM
with `reason=2` also emits `group=vm_fault` (AX…BP, DS, ES, SS:SP) and three
`group=vm_stack part=N` rows with sixteen stack words each. Guest addresses
map to the NASM listings: the stage-1 loader runs at linear 8000h, the DOS
kernel is loaded at 9000h and relocated to 3000h (listing offset + 3000h).
