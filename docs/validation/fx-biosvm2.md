# BIOS state buffers, suspended DOS continuations and APM CMOS

Research checked against the implementation before editing it. The private
T23 disk logs named in the directive were inspected in place.

## Firmware state ownership

[VESA VBE 3.0, function 04h, page 45](https://courses.cs.washington.edu/courses/cse451/20wi/readings/vbe3.pdf)
defines BX as a count of 64-byte blocks, with ES:BX naming the save and
restore buffer and CX=000Fh selecting all state except framebuffer memory.
[X.Org VBESaveRestore](https://fossies.org/dox/xorg-server-26.0.99.903/vbe_8c_source.html)
queries that count and allocates real-mode pages for it; VBEVesaSaveRestore
also keeps a separate copy because other firmware calls may overwrite it.
[Linux uvesafb](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/uvesafb.c)
likewise computes 64*(BX&FFFFh), allocates that length and passes it with
TF_BUF_ESBX to the [v86d helper](https://github.com/mjanusz/v86d).
The [author's uvesafb/v86d protocol description](https://lkml.rescloud.iu.edu/0706.3/2352.html)
specifies the buffer length in each request and an optional returned buffer;
the helper is not a source of a fixed 2 KiB VBE state limit.

CiukiOS currently restricts that BIOS-defined state to the 2 KiB scratch
tail. Reserve 12000h–15FFFh (16 KiB) inside the existing 10000h–1FFFFh BIOS
reservation instead. Check every page with reserved_ram before mapping it;
keep the existing scratch page and 11000h stack unchanged. Add one mapping
record, preserve the old mapping indices, and bound every save/restore by
16 KiB. Use ES=1200h, BX=0 for both transfers, and retain a kernel copy.
Mode-information queries keep using scratch+800h. No allocator limit changes.

## CMOS ownership

The local Windows 98 Resource Kit, chapter 30, wrkc30_4.htm, “Understanding
Advanced Power Management”, assigns APM power control and battery monitoring
to BIOS. The [Intel/Microsoft APM 1.2 specification, sections 4.1–4.2](https://www.scribd.com/document/240491546/Advanced-Power-Management-APM-BIOS-Interface-Specification-Revision-1-2-February-1996)
requires real-mode installation/connect entry (the original is distributed
in [Microsoft's legacy documents](https://www.microsoft.com/en-us/download/details.aspx?id=53426)).
These interfaces do not promise that a real BIOS can operate on a fabricated
RTC cache. [Linux apm_32.c](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/kernel/apm_32.c)
executes BIOS code on CPU 0 with a configurable interrupt policy; it does
not implement a CMOS cache or a 70h/71h port emulator. Linux's separate
[CMOS locking contract](https://raw.githubusercontent.com/torvalds/linux/master/arch/x86/include/asm/mc146818rtc.h)
requires index/data serialization and restoration of an interrupted index.
The inspected Windows documentation does not specify VPOWERD's internal
CMOS port policy; no claim of a Windows CMOS cache/filter is made here.

On this UP kernel the existing RTC lock is a short irq-save section in
fs_port.c, and NMI diagnostics never access CMOS. Reuse that section for
each APM CMOS data access, retaining a software index shadow and restoring
it before releasing the lock. Never read physical port 70h to infer it.
APM OUT 70h changes only its virtual index, including bit 7; each data
transaction applies that index/NMI bit briefly and restores native policy.
IN 70h returns the virtual index. Only byte accesses to exactly 70h/71h
are admitted by the APM callback; input/video/DOS policies stay unchanged.
Never hold the RTC lock across a sleeping or bounded 2-second BIOS call.

## DOS continuation investigation

[DPMI 1.0, default reflection and translation services](https://openwatcom.org/ftp/devel/docs/dpmi10.pdf)
requires the real-mode IRET and host trampoline to restore the suspended
protected-mode caller. A display refusal must not unwind those records or
change virtual IF, VM, client stacks or register results. The DOS virtual
IRQ completion guard currently uses wall time even while the VM is parked;
the VM clock can consequently release held IRQs during a display switch.
Parked time must not consume that guard's guest execution allowance, nor
turn owed PIT/SB interrupts into fresh in-service work while suspended.
Host replay must include a reflected mode-13h call and a held IRQ across a
refused display request, then execute the real IRET/trampoline to PM.

The performance records alone do not identify Duke's exact saved CS:IP or
prove that an interrupted mode set caused its real-mode loop. Hardware
confirmation remains separate from synthetic continuation evidence.

## Host verification

- Production BIOS monitor/broker: 214116 assertions, zero failures,
  including BX=003Bh, the 16 KiB bound, reservation refusal for every new
  page, the exact F000:4633 bytes, RTC/EC interleaving and the combined
  connect/version/soft-off backend replay.
- Production DOS VM suite: 62249730 assertions, including a 32-bit client
  using PM INT10 and 0300h, and a mode set inside a reflected hardware IRQ.
  A 1578 ms refused display transaction preserves the BIOS frame/stack,
  keeps owed IRQs held, and completes IRET/trampoline return to PM.
- Display driver model (11 tests), naming (4), commands (3), mode/checkpoint
  (2), presenter/leases (5), fullscreen runner (3), and both SuperSavage
  C suites exit successfully. All four modified kernel translation units
  compile with the canonical i686 flags and 4096-byte frame limit.
- APM, power and RTC assertions also pass. For these suites, the BIOS
  suite and the DOS suite, LeakSanitizer aborts its final scan because the
  sandbox denies ptrace. Their process exit is 1: this is not a green
  sanitizer result. Leak detection was not disabled. Display/SuperSavage
  tests retain only their repository-provided sanitizer settings.

Exact commands, logs and remaining lead verification are in
`build/codex/fx-biosvm2-report.md`. No QEMU, systemd scope, image build,
commit or diary change was performed.
