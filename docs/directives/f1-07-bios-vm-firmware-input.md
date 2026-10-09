# Directive f1-07: serialized V86 BIOS VM and the firmware-first input backend

- **Step:** F1. **Contracts:** `device-firmware-ownership.md` ("Input, clocks
  and NMI": one controller consumer, firmware-first policy for the E500,
  timeouts disable the backend and never reset blindly; "Firmware-call
  boundary": one serialized V86 BIOS VM with private stack, IVT/BDA working
  state, bounded buffers, explicit mappings of reserved firmware memory,
  IOPL 0, trapped I/O/CLI/STI/PIC/PIT access, virtual IRQ reflection, the
  F1 allowlist = nonblocking INT 16h and INT 15h/C2h input services plus
  their firmware IRQ handlers, deadlines 100 ms status/input and 500 ms
  setup), `dos-dpmi-contract.md` (the V86 monitor model that F3 extends:
  read it so that the F1 monitor is a strict subset of it, not a second
  design), `execution-abi.md` (kernel stacks, TSS, non-preemptible kernel),
  `boot-memory.md` (low-memory reservations, `CBI_F_INPUT_FORCED`),
  `f1-acceptance.md` (`input` on the firmware-first profile; `input-fault`:
  firmware overrun and disallowed I/O cannot mutate the physical PIC/PIT).
- **Implementer:** Codex, `gpt-6-astra`, effort `xhigh`, web search on.
- **Worktree:** `wt/f1-bios-vm`. Files: new `src/kernel/vm/v86.c`,
  `src/kernel/vm/v86_entry.asm`, `src/kernel/vm/biosvm.c`,
  `src/kernel/vm/fwinput.c`, headers `src/kernel/include/ciuki/v86.h`,
  `biosvm.h`, `fwinput.h`; minimal, clearly marked changes in
  `src/kernel/arch/isr.asm`, `src/kernel/arch/trap.c`, `src/kernel/arch/desc.c`
  and `src/kernel/core/task.c` only where V86 frames require them (F0
  behaviour for non-V86 frames must not change); `scripts/build_kernel.py`
  (add the `vm` directory and `vm/*.asm`); `tests/host/v86_test.c`;
  `scripts/test/host_kernel_tests.sh`. Do not touch probes, drivers, the
  loader, the runner or contracts.

## What to build

1. **V86 monitor core** (`v86.c`, `v86_entry.asm`): a kernel thread enters
   V86 mode by building an IRET frame with `EFLAGS.VM=1`, IOPL=0, the
   real-mode CS:IP and SS:SP and the four data segments; exceptions and
   interrupts taken in V86 arrive on the thread's kernel stack with the
   extended frame (ES, DS, FS, GS after SS:ESP) — `isr.asm`/`trap.c` must
   recognise `VM=1` in the saved EFLAGS and route #GP, #UD, #PF and IRQs to
   the monitor without changing the F0 paths. The #GP emulator handles,
   with operand-size and segment prefixes as needed: `INT n` (reflect
   through the VM's IVT with a real-mode frame), `IRET`, `PUSHF`/`POPF`
   (virtual IF only), `CLI`/`STI` (virtual IF), `IN`/`OUT`/`INS`/`OUTS` in
   all widths (port policy below), `HLT` (yield until a virtual interrupt
   is pending or the call deadline passes), `INT3`/`INTO` as reflected
   interrupts. Anything else terminates the call with `-EFAULT` and a
   record of CS:IP and opcode bytes. The virtual IF, pending virtual IRQs
   and the IRET-to-caller detection (a sentinel return address) are kept
   per VM. The monitor never runs with host interrupts disabled for more
   than the F0 budget.
2. **Port policy**: an allowlist table per VM. For the BIOS input VM:
   0x60/0x64 pass through to the real controller (the lease), 0x20/0x21 and
   0xA0/0xA1 go to a **virtual PIC** (mask reads/writes and EOIs affect only
   the virtual PIC; the physical PIC is owned by the kernel dispatcher),
   0x40–0x43 and 0x61 are virtualised read-only to a PIT model fed by
   `g_ticks` (no physical PIT writes), 0x70/0x71 return the kernel's cached
   RTC values and ignore writes, everything else is refused: the call ends
   with `-EPERM` and `disallowed_io++`. No port write reaches the hardware
   from the VM except 0x60/0x64.
3. **Memory of the BIOS VM**: a private page directory mapping the first
   MiB as the VM sees it: the real IVT and BDA pages (0x0000–0x04FF) and the
   EBDA (from BDA 0x40E) mapped to their physical pages (firmware working
   state), the system ROM 0xF0000–0xFFFFF and video/option ROMs
   0xC0000–0xEFFFF mapped read-only from physical, a private stack page and
   a private scratch page for the monitor's stubs (the mouse packet
   handler and the IRET sentinel) in low RAM pages reserved through the PMM
   (`boot-memory.md` reservations), everything else unmapped. The kernel
   mapping at 0xC0000000 stays in the same directory (PDEs 768–1022 shared).
4. **Serialized firmware calls** (`biosvm.c`): `biosvm_call(struct
   biosvm_regs *regs, uint32_t deadline_ms)` runs one real-mode interrupt
   (`INT n` with the given registers) under a firmware `kmutex`, with the
   contract deadlines (100 ms status/input, 500 ms setup); a call that
   exceeds its deadline is aborted: the VM thread is parked, the backend is
   marked `disabled`, the lease resources are quarantined in the registry,
   and every later call returns `-EIO` without entering the VM. The
   registry lock is never held while the VM runs. Counters: calls,
   trapped I/O by port class, reflected IRQs, disallowed I/O, timeouts,
   emulated instruction classes.
5. **Firmware IRQ reflection**: when the firmware-first backend is enabled,
   the kernel claims IRQ1 and IRQ12 (and ports 0x60/0x64) in the registry
   as the `firmware-input` lease and installs handlers with
   `irq_set_handler` that record a pending virtual IRQ (vector 9 and 0x74
   via the virtual PIC) and wake the VM thread; the dispatcher still does
   the physical EOI. The VM thread delivers the virtual IRQ by reflecting
   through the IVT with host interrupts enabled; the BIOS ISR reads the
   controller and updates the BDA. The native i8042 driver is not started
   on this boot (it already refuses when `CBI_F_INPUT_FORCED` is set).
6. **Firmware input backend** (`fwinput.c`): after each reflected IRQ1 and
   on a 10 ms poll, `INT 16h AH=11h` then `AH=10h` drain the BIOS keyboard
   buffer into a bounded ring of `struct fwinput_event {type, code,
   value, tick}` (key make/break derived from the scan code in AH with a
   stable code table; BIOS translation set 1 codes are documented); the
   mouse uses `INT 15h AH=C2h` (reset, set sample rate, set resolution,
   enable, device-handler far pointer to the stub in the scratch page)
   whose stub stores 3-byte packets into a ring in the scratch page that
   `fwinput_poll` decodes into relative motion and button transitions.
   Setup (500 ms deadline) records which functions succeeded; any
   unsupported function disables the mouse half, not the keyboard half.
   Expose `fwinput_poll(struct fwinput_event *out, unsigned max)`,
   `fwinput_stats` (loss, resyncs) and `fwinput_backend_state`; the
   adaptation to the native input queue of f1-04 is done by the lead's
   integration directive.
7. **Self-check** `biosvm_selftest(struct biosvm_selftest_report *r)` used
   later by the `input-fault` probe: run a scripted real-mode stub in the
   scratch page that attempts `OUT 0x21`, `OUT 0x43`, `CLI`/`STI`, `INT 1Ch`
   and a `HLT` loop past the deadline, and prove that the physical PIC mask
   and PIT state are unchanged (read back by the kernel), the disallowed
   port was refused, and the deadline aborted the call and disabled the
   backend (then re-enable for the test only through a documented
   `biosvm_reset_for_test`, never in production).

## Host tests (mandatory)

`tests/host/v86_test.c` under ASan/UBSan: the #GP decoder on byte
sequences for every emulated instruction with prefixes (frame updates,
virtual IF, IVT reflection frames, IRET sentinel), the port policy table
and virtual PIC/PIT models (physical-write counter stays 0 except
0x60/0x64), the deadline/abort state machine, the fwinput decoders
(set 1 scan codes incl. E0 prefixes, mouse packet validation and
resync). Scheduler/CPU-dependent parts are isolated behind small
functions with fakes, as in `kernel_sync_test.c`.

## Acceptance by the lead

Diff review; host tests; kernel build with the FPU audit; the F0 suites
unchanged on all profiles (the V86 paths are dormant without
`CBI_F_INPUT_FORCED`); on `qemu-e500` (SeaBIOS, firmware-first forced) the
backend sets up, keystrokes and mouse packets injected through QMP arrive
in `fwinput_poll`, the self-check passes, and a forced timeout disables the
backend without touching the physical PIC. Reply with: files, interfaces
(signatures), test output, the list of firmware functions used, and any
contract problem found.
