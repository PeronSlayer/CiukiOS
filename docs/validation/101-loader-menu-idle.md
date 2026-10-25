# 101-c: loader menu interrupt idle

The loader runs the menus in BIOS real mode: `mbr.asm` preserves the IVT,
`ciukldr.asm:entry` establishes SS:ESP and enables interrupts before discovery,
and the loader masks the PIC only after the menus, at the protected-mode
handoff. The A20 probe saves/restores FLAGS. No menu changes the IVT or PIC.
This is the existing BIOS IRQ0/IRQ1 environment, not a kernel idle routine.

## Sources and implementation decision

- [RBIL INT 16h/AH=01h](https://delorie.com/djgpp/doc/rbinter/id/64/17.html)
  checks availability without consuming a key. [AH=00h](https://www.delorie.com/djgpp/doc/rbinter/id/63/17.html)
  reads the character and scan code. Keep that pair so Ctrl/F8, the other
  function keys, serial input and timed defaults remain independent.
- [RBIL INT 15h/AH=86h](https://www.delorie.com/djgpp/doc/rbinter/id/33/15.html)
  is an AT/PS wait with firmware-dependent resolution and an error result
  when another BIOS wait is active. It adds an unnecessary RTC/firmware
  dependency; use the already-running BIOS timer instead.
- [Intel SDM, HLT](https://cdrdv2-public.intel.com/812383/253666-sdm-vol-2a.pdf)
  specifies that an enabled interrupt resumes a halted CPU. Execute adjacent
  `sti; hlt` on every empty input pass, even if firmware returned with IF
  clear. Leave IF enabled on return. The BIOS IRQ0 normally wakes the CPU
  within one tick; IRQ1 can wake it sooner. A key arriving between the check
  and halt remains buffered and is read on the next pass, at most one timer
  tick later. Drain already-available input without sleeping between bytes.
- [Microsoft's MS-DOS 4 console, MSCON.ASM:CHRIN](https://github.com/microsoft/MS-DOS/blob/main/v4.0/src/BIOS/MSCON.ASM)
  delegates blocking reads to BIOS INT 16h (AH=00h or 10h). This is evidence
  for the BIOS interface, not a claim about proprietary Windows 98 IO.SYS
  internals. [SeaBIOS kbd.c:dequeue_key](https://github.com/coreboot/seabios/blob/master/src/kbd.c)
  waits through `yield_toirq`; [stacks.c:wait_irq](https://github.com/coreboot/seabios/blob/master/src/stacks.c)
  uses STI/HLT. [GRUB term.c:grub_getkey](https://github.com/rhboot/grub2/blob/master/grub-core/kern/term.c)
  similarly checks nonblocking input and idles only when none is available.
- The owner's Windows 98 Resource Kit, chapter 5 `wrkc05_4.htm` (Msdos.sys
  BootDelay/BootKeys/BootMenu options) and chapter 27 `wrkc27_3.htm` (Startup
  menu procedures), confirms the user-visible startup delay and held Ctrl
  behavior. It does not establish which instructions IO.SYS uses to wait.
  Those local files were consulted read-only; no media or extracted text is
  redistributed. Keep the existing CiukiOS hotkeys and timeouts.
- [QEMU invocation, -icount](https://qemu-project.gitlab.io/qemu/system/invocation.html)
  specifies instruction-driven virtual time and real-time advancement during
  CPU sleep with `sleep=on`. HLT enables the countdown to advance without
  executing a busy loop. Wall-clock timing remains a lead-run QEMU check.

`menu_polls` becomes a saturating idle-wakeup budget of twice BP (the requested
BIOS ticks), consumed only after HLT returns, never by a deadline check or a
buffered input byte. Under ordinary IRQ0 wakeups this is a two-window tick
safety bound (110 waits for the 55-tick boot menu; 1096 for command entry).
The BDA elapsed-time deadline remains authoritative, including midnight wrap.
The spare window tolerates keyboard/other IRQ wakes; those wakes also consume
the fallback budget, so it is an interrupt-wait bound, not an independent
physical clock. If BDA updates stop but IRQs continue, it still expires. If
all wake sources stop, software cannot bound a single HLT. No new fatal or
panic path is introduced.

## Serial limit requiring lead awareness

`uart_init` leaves IER zero, sets divisor 3 (38,400 baud, 8N1), and does not
enable FCR. The [TI PC16550D data sheet](https://media.digikey.com/pdf/Data%20Sheets/Texas%20Instruments%20PDFs/PC16550D.pdf)
describes a 16-byte receive FIFO only when enabled. Even with that FIFO,
16 * 10 / 38400 is about 4.17 ms, less than a 54.9 ms BIOS tick. Thus the
directive's assertion that the FIFO covers 55 ms cannot guarantee arbitrary
full-speed bursts on hardware. This change preserves polled COM1 selection,
including immediately draining bytes already present; no UART IRQ handler,
baud policy or flow control is added. Full-speed serial burst guarantees need
a separate lead decision. Host serial fixtures prove control flow and preserve
queued bytes; they do not simulate hardware FIFO capacity or wire timing.
