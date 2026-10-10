# Directive f3-01: per-VM PIC, PIT, keyboard and mouse devices for DOS VMs

- **Step:** F3. **Contracts:** `dos-dpmi-contract.md` ("V86 monitor and
  BIOS boundary": one authoritative virtual PIC per VM, IRR/ISR/IMR,
  priorities, cascade, ICW/OCW, EOI, guest bases `08h/70h`, STI/MOV-SS
  shadow; "Virtual devices and reuse": per-VM PIT at 18.2 Hz tracking guest
  programming, keyboard focus, INT 33h mouse, focus loss releases held
  keys/buttons; acceptance row "F3 monitor"), `device-firmware-ownership.md`
  (virtual IRQ inputs; physical EOI never waits for the guest), directive
  f3-00 (the `dosvm.h` API: `dosvm_port_register`, `dosvm_irq_raise`,
  `dosvm_keyboard`, `dosvm_tick`, `dosvm_read/write`).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-vm-devices`, from `main` after f3-00 is merged.
  Files: new `src/kernel/vm/dosvm_pic.c`, `dosvm_pit.c`, `dosvm_kbd.c`,
  `dosvm_mouse.c` (models migrated from `src/vm/guest_peripherals.c`:
  `cvgp_init`, `cvgp_io_read/write`, `cvgp_advance`, with the `[F3]`
  corrections of the device contract, e.g. slave spurious IRQ15, host time
  first), the per-VM wiring in `dosvm.c` only through the f3-00 API, the
  desktop input routing hook (a focused DOS window forwards key/motion/
  button events to its VM through a kernel interface added here and used
  by f3-02), `src/kernel/probes/f3_probes_devices.c` registered in
  `build_kernel.py`, `tests/suites/f3-vm.json` additions, host tests
  (port the model tests of `src/vm/`), `docs/design/f3-acceptance.md`
  (section `## f3-01`).

## Scope

1. **PIC.** Master/slave 8259A per VM: ICW1–4, OCW1–3, IRR/ISR/IMR,
   fully nested priorities, specific/non-specific EOI, auto-EOI, rotation
   where the contract requires, cascade on IRQ2, spurious IRQ7/IRQ15,
   guest-programmed vector bases; delivery only with virtual IF, PIC
   eligibility and no STI/MOV-SS shadow (`v86.c` monitor hooks); guest
   CLI never blocks host preemption; host vectors and masks untouched.
2. **PIT.** Three channels per VM, modes 0–5, latch/read-back, BCD
   flag, gate for channel 2 (speaker bit of port `61h`), default 18.2 Hz
   IRQ0, tracking guest reprogramming independently of the host's
   1000 Hz tick via `dosvm_tick` (bounded catch-up with a recorded cap).
3. **Keyboard.** Virtual 8042/keyboard per VM (ports `60h/64h`, output
   buffer, status, command byte, scancode set 1 translation, typematic
   from host events, LEDs ignored), fed by the focused desktop window's
   events; INT 09h via IRQ1; BIOS keyboard buffer (`INT 16h`) stays in the
   f3-00 BIOS; focus loss releases all held keys.
4. **Mouse.** INT 33h driver semantics (reset/status, position and
   buttons, motion counters, cursor show/hide counters, event mask and
   user callback with the contract's V86 call path, text cursor) per VM,
   fed by the focused window's motion/buttons; focus loss releases
   buttons; no physical PS/2 passthrough.
5. **Probe `f3:vm-devices`**: a DOS-side payload (record port) programs
   the PIT and PIC, counts ticks against the VM clock, tests masks/EOI
   order, CLI loop preemptibility (host heartbeat continues), STI shadow
   boundary, HLT wake, malformed I/O locality, keyboard/mouse injection
   through the kernel interface, focus-loss releases; records per the
   "F3 monitor" acceptance row. Cases on t23/e500/min128.
6. **Host tests:** the migrated model tests plus the new rules.

## Acceptance by the lead

Host tests; kernel build; QEMU `f3-vm` devices cases PASS; `vm-boot`
unchanged; `f2-all` unaffected.
