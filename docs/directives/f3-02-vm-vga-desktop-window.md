# Directive f3-02: per-VM virtual VGA and the DOS window on the desktop

- **Step:** F3. **Contracts:** `dos-dpmi-contract.md` ("Virtual devices
  and reuse": VGA text, planar modes, mode 13h/Mode X per VM; INT 10h
  virtual; guest VBE unavailable through F4 with the VBE status
  convention), `f2-acceptance.md` and `docs/desktop.md` (ring-3 desktop,
  surfaces, protocol v2 of f2-30), `device-firmware-ownership.md` (no
  physical mode change by a guest), directive f3-00 (`dosvm_text`,
  `dosvm_port_register`, memory helpers) and f3-01 (input routing).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-vm-vga`, from `main` after f3-00 is merged.
  Files: new `src/kernel/vm/dosvm_vga.c` (models migrated from
  `src/vm/virtual_vga.c`, `virtual_vga_bios.c`, `vga_x86.c`: `cvga_init`,
  VRAM access and rendering, per-VM instances, Jemm/HDPMI adapters
  replaced by `dosvm_read/write`), the VRAM aperture `A0000–BFFFF` mapped
  per VM to private pages with the planar/chain-4 write modes emulated or
  trapped as the models require, INT 10h in the f3-00 BIOS extended to
  the model (modes 0–3, 13h, text cursor, palette DAC ports `3C6h–3C9h`,
  CRTC/sequencer/graphics/attribute registers), a kernel presenter that
  renders the VM's frame into a desktop surface (`vga_presenter.c`
  logic) at a bounded rate; a new desktop client `/bin/dosbox` (ring-3,
  SDK) that owns one DOS window: creates the VM through a supervisor-
  delegated capability (syscall or channel added here, recorded in
  `execution-abi.md`), presents its surface, routes focused input to the
  VM (f3-01 interface) and closes the VM on window close; the Ciuki menu
  entry comes from `/bin` as f2-30 defines. Probe
  `src/kernel/probes/f3_probes_video.c`; `tests/suites/f3-vm.json`
  additions; host tests (migrated `test_virtual_vga.c`,
  `test_vga_x86_shim.c`); `docs/design/f3-acceptance.md` (`## f3-02`).

## Scope

1. Text mode 3 with attributes, cursor and blink, 80×25 and 80×50;
   graphics modes 13h, Mode X (unchained 320×240/200), planar 12h/10h as
   the models already support; DAC palette; retrace status bit `3DAh`
   tied to the presenter's frame clock; no VBE (`AX=014Fh` failure
   convention) and no physical register access.
2. Presenter: VM frame → desktop surface (32-bit), bounded to the
   desktop's frame rate, dirty-region tracking, text rendered with the
   existing 8×16 font, aspect handling by integer scaling only.
3. `/bin/dosbox`: window titled with the VM's name, close → clean VM
   termination, focus → input routing, VM exit → window closes; one
   process per DOS window; crash of `dosbox` terminates its VM through the
   supervisor (idempotent cleanup of f3-00).
4. Probe `f3:vm-video`: a DOS-side payload writes known text and
   graphics patterns (mode 3, 13h, Mode X), the probe hashes the
   presenter's frame buffer and compares with golden digests computed by
   the host test from the same patterns; palette and retrace checks;
   cases on t23/e500/min128; the desktop profile cases open the window
   through `/bin/dosbox` and verify a `desktop_screen` digest region.
5. Host tests: migrated VGA model tests, presenter digests.

## Acceptance by the lead

Host tests; kernel, SDK and image build; QEMU `f3-vm` video cases PASS
with the golden digests; a runner screenshot of a DOS window on the
desktop for the owner; `f2-all` unaffected.
