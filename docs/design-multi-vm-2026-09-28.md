# Design: several DOS windows at once (multi-VM) — 28 September 2026

Status: M1 and M2 complete and passing on QEMU
([evidence](validation/2026-09-28-multi-vm/README.md)). M3 complete and
passing on QEMU: every item below passes its gate, and the complete profile
passes 21/21 ([evidence](validation/2026-09-29-boot-and-m3/README.md),
section 5). M4 passed its QEMU profile on 30 September; M5 remains open. Part of
[the roadmap](roadmap-dos-vm-desktop-2026-09-28.md), phase 2.

## Implemented (M1, M2)

The VM manager lives in CVSESSION (`src/vm/session_vmm.inc`, operations
40h-43h in `src/vm/session_abi.inc`). It builds on Jemm's IRQ0 host-scheduler
callback and the per-VM interrupt state (profile functions 9/10 in
`patches/jemm-ciukios-vm-scheduler.patch`).

- **Private memory.** Linear pages 00h-9Fh are private per VM: the IVT, the
  BIOS data, the kernel and the whole DOS arena. Each VM has its own kernel
  instance, as the kernel's Windows instance table asks for. UMBs, ROMs and
  extended memory are global.
- **Creation by fork.** `\VM\VMFORK.COM program [args]`, run by the system
  VM, calls `VMM_INIT` (kernel layout check) and then `VMM_CREATE`.
  - The first megabyte is copied as it is at that call, so the kernel's
    allocator block table and EXEC frames are coherent in both copies. An
    arena built from nothing was not: the kernel keeps its own allocator
    state beside the MCBs.
  - The system VM gets CX=0 and VMFORK exits.
  - The new VM continues after the same call with CX=1. It frees its
    ancestors' blocks (the desktop is never resumed there), EXECs the
    program and reports the exit code with `VMM_EXIT`. It then idles in HLT
    until the manager releases it.
- **Kernel layout.** `scripts/build_vm_session.sh` takes the kernel listing
  and writes `kernel_layout.inc` (InDOS, FAT cache flags, last INT 21h
  function). `VMM_INIT` refuses a kernel whose InDOS offset differs.
- **Switch.**
  - Every two IRQ0 ticks, round robin. The switch swaps:
    - the 76-byte client frame, keeping the interrupt being reflected;
    - the 160 PTEs, with a CR3 reload;
    - Jemm's per-VM virtual PIC/IF state (fn 9/10).
  - A VM entered for the first time inherits the leaving VM's PIC bases and
    masks with nothing pending. An all-zero PIC would turn IRQ0 into INT 0.
  - The callback runs from the canonical V86 frame at the top of Jemm's
    stack. It also runs from ring 0 while Jemm emulates a guest HLT (IRQ0
    during `EnableInts`): that frame is then the #GP of the HLT, and Jemm
    returns to V86 through it. That is how an idle or finished VM gives its
    time away.
- **DOS is entered by one VM at a time.** A VM is left only:
  - when InDOS is 0 and its one-sector FAT write-back cache is clean;
  - or when it waits in HLT inside a keyboard-input call (INT 21h 01h,
    06h-08h, 0Ah, 0Ch), which leaves no file-system change half done.

  The resumed VM's FAT cache is invalidated, so it re-reads what the others
  wrote. The directory search cache is reset by every search. No other disk
  state is cached across INT 21h calls.
- **Gate:** `scripts/qemu_test_vmm.py` covers switching, the key wait inside
  DOS and file I/O from two VMs, then checks the files and `fsck.fat` from the
  host.

## M3: the DOS window session in its own VM (done)

First part:

The session (virtual VGA, device model) belongs to the VM that runs BEGIN
(`session_vm`). The desktop keeps the real screen and its own input.

- **Per-VM hooks.** Every hook acts only in the session's VM:
  - VGA and device port traps;
  - the INT 10h hook;
  - the #PF hook;
  - the timer presenter;
  - the session scheduler service.

  In the other VMs the ports reach the hardware through Jemm's
  `Simulate_IO`, and INT 10h reaches the desktop's own path. END is
  accepted only from the session's VM. VMM_EXIT refuses while that VM still
  holds the session. A fork is refused while the calling VM holds one, since
  its trapped aperture would be copied.
- **Per-VM aperture.** The PTE set grows to C0h pages: each VM keeps its own
  A0000h-BFFFFh mapping (the desktop's LFB bank window or the session's
  trapped shadow). A switch saves the leaving VM's entries first, because
  they change at run time.
- **Interrupts to the right VM.** `vmm_pend` raises a virtual IRQ in Jemm's
  live PIC when that VM runs. Otherwise it sets the line pending in the VM's
  saved PIC state, which is injected on its next slice. The device model's
  IRQs always go to the session's VM.
- **Keyboard and mouse.**
  - One Jemm IRQ1/IRQ12 filter (`jlm_filter`) is installed while the VM
    manager or the device model runs.
  - The keyboard goes to the focus VM (`VMM_FOCUS`, op 44h). For the
    session's VM, the device model takes the physical bytes. Any other VM
    gets its line pended and reads the controller itself.
  - The mouse goes to the focus VM when its session has a mouse, otherwise
    to the system VM (the desktop); see "Several sessions at once" below.
  - `cvdev_set_pull` keeps the device model from taking bytes that belong
    to another VM. A VM that loses the focus has its held keys released.
- **One IRQ0 callback** (`host_tick`) serves the session scheduler and the
  VM manager. Jemm has one host-scheduler slot, and `VM_ERROR_VMM_BUSY` no
  longer happens with a window open.
- **Gate:** `scripts/qemu_test_vmm.py`, third part (`VMWTEST`/`VMWCHILD`).
  VM 1 begins its own session and text mode. The system VM reads VM 1's
  screen, gives it the keyboard ("abc") and takes it back ("x"), then closes
  VM 1 with Esc sent through its device model. Each VM gets exactly its own
  keys, and VM 1's text never reaches the physical screen.

## M3: done since, each with its gate part in scripts/qemu_test_vmm.py

1. **Sound Blaster DMA of a VM that is not running** (`sound`). The device
   model reads ISA DMA memory of the session's VM through a one-page window
   onto that VM's saved page frames (`cvdev_guest_linear`). Gate: VM 1 plays
   a 220 Hz square wave while the system VM computes. The recorded output is
   that wave (100 % of samples on its level, 439 crossings/s). Before the fix
   it was not (22 %, 1000 crossings/s).
2. **Vectors of a forked VM** (`ivt`). CVSESSION keeps the interrupt vectors
   as they were when it loaded (`VMM_IVT`, 46h). VMFORK gives those values
   back, in the new VM, to every vector that points into the blocks it frees
   there. Gate: VMITEST hooks INT 1Ch/09h in its own block. VM 1 finds them
   restored, overwrites that block and runs 40 ticks, and the system VM's
   hooks keep counting. The previous VMFORK fails this gate.
3. **Clocks** (`clock`).
   - Every VM is owed the IRQ0 ticks that reach another VM, and gets them one
     at a time when it runs again (`jlm_poll`, the shared return-to-V86
     callback).
   - The CMOS index register (70h) is kept per VM: 70h/71h are trapped while
     the VM manager is armed, and an access to 71h selects the VM's own index
     first.
   - Gate: two busy VMs count 182 and 181 BIOS ticks over 10 s of CMOS time.
     Without the delivery each gets half (91). Without the per-VM index the
     two CMOS readers corrupt each other's index.
4. **Ending a session owner** (`kill`).
   - VMM_EXIT of the VM that owns the DOS window session ends the session.
   - VMM_KILL (45h) ends another VM and its session. The teardown runs with
     that VM's page table entries mapped (`vmm_force_end`), none of its code
     running.
   - Gate: VMs that exit without END, end properly, or hang with CLI and are
     killed. The session is free afterwards and the next key is intact.
5. **Focus release hotkey** (`window`). Ctrl+Esc in the session's VM gives the
   keyboard back to the system VM. The VM sees neither key. `VMM_STATE`
   reports the focus.

Found and fixed on the way:
- **The first key after a DOS window session with the keyboard came out
  wrong** (Phase 1 too). Jemm traps port 60h only for one access after
  D0h/D1h (A20 through the controller). JLOAD's `Remove_IO_Handler` left
  that IOPB bit set, so the next read of 60h went to Jemm's output-port
  emulation, which forces the A20 bit (z 2Ch became c 2Eh). The device model
  now clears the bit when its traps go.
- **Two VMs reading the CMOS clock** corrupted each other's index (item 3).

## M3: several DOS window sessions at once

Each item has its gate part in `scripts/qemu_test_vmm.py`.

1. **One VGA and device model per VM** (`multi`).
   - The C models keep their state in instance blocks (`cvvid_instance`,
     `cvdev_instance`), one per VM that begins a session. A switch selects
     the next VM's blocks (`vmm_select`).
   - CVSESSION's own per-session variables are listed in `sess_ctx`. They
     are live for the running VM and saved in every other VM's record
     (`ctx_save`/`ctx_load` at a switch).
   - Port traps, the INT 10h, #PF and timer hooks and the IRQ1/IRQ12 filter
     are shared. They are installed with the first session, counted per
     port, and removed with the last. A trapped port of a VM without a
     session reaches the hardware (`Simulate_IO`).
   - `VMM_TARGET` (47h): the system VM addresses another VM's session for
     QUERY, READBACK, the VIDEO_* reads, DEV_STATE, DEV_KEY and DEV_MOUSE.
     That VM's variables and blocks are live for the call.
   - Gate: VMDTEST forks two VMWCHILDs, each with its own session and
     keyboard. Both screens are read apart. a reaches VM 1 only and b VM 2
     only. Ctrl+Esc gives the keyboard back, x reaches the system VM, and
     Esc ends each VM through its own model. Neither VM's text reaches the
     physical screen.
2. **Mouse routing** (`mouse`).
   - Physical IRQ12 goes to the focus VM when its session has a mouse
     (`vmm_line_target`). That VM's model takes the bytes, also when it runs
     later (`pull_lines`).
   - DEV_MOUSE (37h) feeds the addressed session.
   - Gate: VMOTEST forks two VMOCHILDs. Each installs a BIOS pointing-device
     handler (INT 15h C205h/C207h/C200h) on its own model.
     - The physical mouse moves right with VM 1 focused: VM 1 gets X +12,
       VM 2 nothing.
     - It moves down with VM 2 focused: VM 2 gets Y -12, VM 1 nothing more.
     - DEV_MOUSE into VM 1 arrives exactly (+5, +7, left button), and VM 2
       gets nothing.
3. **Audio ownership** (`audio`).
   - There is one AC'97 stream. It plays the focus VM's session if that one
     plays, else the first session that plays (`vmm_pick_audio`).
   - The other sessions play muted. Their SB16 DMA and OPL go on in real
     time into a discarded buffer, whichever VM runs, so a background
     program never waits for its sound IRQs.
   - Gate: VMATEST runs VM 1 (220 Hz) and VM 2 (551 Hz) at once and holds
     each state for 3 s. The recorded output of each state is the owner's
     wave alone (100 % on level; 440 or 1099 crossings/s):
     - focus VM 1;
     - focus VM 2;
     - focus on the system VM, where VM 1 plays;
     - VM 1 ended, where VM 2 plays.
   - Each VM played its blocks in real time (ratios 0.98 and 0.98; the last
     block is unfinished).

Found and fixed on the way:
- **Two VMs waiting in INT 16h at once swapped registers.** SeaBIOS runs
  INT 16h on one global extra stack, and keeps its stack position in RAM at
  E000h-EFFFh ("low" data, page EEh). A VM left waiting inside the BIOS had
  its saved state overwritten by the next VM's call, and resumed with that
  VM's registers.
  - The VM manager now finds the BIOS RAM in upper memory at start: pages
    C0h-EFh that are identity-mapped, writable and hold what is written.
  - Each VM gets its own copy of those pages, like its first 640 KB, and a
    switch swaps their entries (`bios_ram_scan`, `bios_ptes_in/out`).
  - Jemm's UMBs and page frame are not identity-mapped and stay shared.
- **A key typed just before the focus moved was lost.** A focus loss
  dropped the model's queued keyboard bytes, though the VM had not run yet.
  Those bytes now stay the VM's, with the breaks for held keys after them.
  They are dropped only when there is no room for those breaks.
  `scripts/test_guest_peripherals.py` covers this case.
- **Muted sessions lost time.** They advanced only in their own slices, at
  most 200 ms at a time: 0.86 of real time. They now advance from the
  shared audio poll, as the owner does.

## Scope after M3 (historical plan)

- **M4.** The desktop paints each VM's session into its window
  (`VIDEO_BAND` from the system VM), sets the focus on clicks, and the
  DOS-window manager moves out of SHELL.COM.
- **M5.** DPMI hosts per VM.
- **Not confirmed: the suspected CiukiDOS AH=48h overlap.** While testing
  M3, an early VMICHILD seemed to get memory inside its own block. The
  addresses behind that diagnosis were set by the test harness, and VMFORK
  has changed since. The DOS memory gate (`qemu_test_full_dos_memory.py`)
  now checks that AH=48h blocks, including the largest one, lie above the
  caller's own block:
  - from the shell;
  - in a nested EXEC child;
  - in a VM forked by VMFORK after it freed its ancestors' blocks.

  All three pass on the M3 image.

## M4 integration state on 30 September 2026

The desktop now loads `DOSVM.APP` as its DOS-window manager. It creates a
forked VM with `VMFORK.COM` for each window and starts `DPMIRUN.COM /V` inside
that VM, where the session and DPMI host belong to the guest. `DOSVM.APP`
targets each guest through CVSESSION and presents its video into compositor
bands. Its video-damage poll queues only the client rectangle, then asks the
shell module host to paint that rectangle. Input focus is set through
`VMM_FOCUS`; the desktop remains VM 0. The former synchronous callback DOS
window manager has been removed from the shell image.

The focused QEMU gate passed on the final clean image: DOOM gameplay while
Files is open, two live text VMs, click focus, keyboard routing, and a
bounded close that leaves Files and the other VM alive. The shipped
full-screen DOS path also returned to its prompt in QEMU. The older
16/25 profile was a development snapshot with harnesses tied to removed
shell symbols. The replacement behavioral gates cover text, VGA, mouse,
audio, DPMI, Task Manager and two simultaneous VMs. The complete 0.8.0
profile passes **26/26** gates at
`build/tests/vm-window-profile-080-final-20260930/SUMMARY.json`.
M4 is complete for its QEMU scope. The evidence and limitations are in
`docs/validation/2026-09-30-m4/README.md`.

## Where we start

- Jemm386 is a V86 monitor for **one** V86 context: one set of page tables
  for the first megabyte and one client register frame. CVSESSION adds one
  virtual VGA model, one device model and one scheduler descriptor.
- A DOS window today is not a VM of its own. DOSWIN EXECs the program as a
  child of the desktop process (SHELL.COM). The desktop keeps running
  through timer "host periods" called from inside the guest's timeline. That
  is why only one window can hold a program: EXEC is nested and synchronous.
- The CiukiDOS kernel already carries what Windows 3.x DOSMGR needed to share
  one DOS between VMs: the DOS 4+ swappable data area (`AX=5D06h`,
  `5D0Bh`), the InDOS/critical-error flags, and PSP creation through `AH=55h`.
  Windows 3.1 386 Enhanced Mode ran its DOS VMs on it.

## Target model (Windows 3.x/95 VMM)

- **System VM**: the desktop (SHELL.COM, kernel, drivers). It is VM 0 and is
  scheduled like the others.
- **DOS VMs**: each window is a VM. Each VM gets:
  - its own copy of conventional memory above a global line (the DOS kernel,
    SYSVARS, device drivers and the VM manager's resident part stay global
    and shared);
  - its own HMA/UMB instance pages;
  - its own register frame, IF/PIC state, virtual VGA model, keyboard/mouse
    queue and DPMI host;
  - a window in the desktop.
- **DOS is global and entered by one VM at a time**:
  - InDOS / a DOS critical section held by the VMM;
  - the SDA swapped on every VM switch, as Windows DOSMGR does;
  - file-system state (SFT, CDS, buffers, FAT) is global, so there is one
    owner of every disk structure.
- **Scheduling**: pre-emptive time slices from IRQ0 in the monitor. A VM
  blocked in DOS or waiting for input yields. The focused window gets a
  larger share (Windows' foreground/background priority).
- **Devices**:
  - VGA is virtual per VM;
  - keyboard and mouse go to the focused VM;
  - SB16/OPL are mixed from every VM into the AC'97 stream (per-VM model
    instances), or owned by the focused VM in a first step.
- **Protected mode**: each VM's DPMI host (HDPMI, VCPI client) runs while
  its VM is scheduled. A switch away from a VM that is inside its DPMI host
  goes through the host's existing scheduler hook (`cvdpmi_scheduler_tick`)
  back to V86 and the monitor, which saves that VM's full state.

## Where the switch goes

Jemm keeps the V86 client frame at a fixed top of its ring-0 stack (`?TOS`),
and IRQ0 from V86 already calls the exact-owned host scheduler
(`pHostScheduler`, CVSESSION's scheduler) before reflection. That callback is
a clean boundary. Swapping the frame at `?TOS` there switches the V86
context, so the VMM can live in the JLM on top of the existing hooks rather
than in Jemm's core. A switch:
1. saves the running VM's frame and first-megabyte PTEs;
2. saves its SDA through the kernel's `5D06h` layout;
3. loads the next VM's frame, PTEs and SDA and flushes the TLB.

It happens only when InDOS is 0 (DOS is not re-entered). IO-trap handlers,
fault hooks and the profile PIC consult the current VM.

## Work packages

1. **Global/instance memory map**:
   - measure what the kernel, SHELL and drivers occupy;
   - define the global line;
   - build per-VM page sets (copy on creation);
   - add a monitor service to switch the first-megabyte PTEs per VM.
2. **VM control blocks in the monitor**:
   - creation (fork of the system VM's global part plus a fresh arena with a
     COMMAND.COM process), destruction, register-frame save/restore;
   - IRQ0 time slicing;
   - per-VM virtual PIC and IF (the negotiated profile made per VM).
3. **DOS serialization**:
   - INT 21h/25h/26h/2Fh entry is a critical section;
   - SDA swap on VM switch;
   - per-VM current PSP, DTA and drive, through the kernel's existing SDA.
4. **Per-VM devices in CVSESSION**:
   - N VGA model instances with their shares;
   - focus routing of keyboard/mouse;
   - audio ownership or mixing.
5. **Desktop**:
   - N DOS windows, task buttons, focus and close;
   - the DOS-window manager moves out of SHELL.COM (96 bytes free) into a
     separately loaded module.
6. **DPMI per VM**:
   - one bound HDPMI per VM;
   - pre-emption through the host scheduler hook;
   - tests with two DOS/4GW games at once.
7. **Gates**:
   - two and three concurrent windows (real-mode programs, then DOS/4GW);
   - file I/O from two VMs at once;
   - focus/audio, close and kill of a hung VM.

## Risks

- Jemm is not a multi-VM monitor. The switch must be added under its V86
  handlers, not beside them; every hook (IO traps, fault hooks, profile PIC)
  becomes per-VM.
- DOS/4GW/HDPMI pre-emption is the hardest part. HDPMI owns the CPU and the
  IDT while its client runs.
- Real hardware remains unqualified; QEMU is the reference.
