# Directive f3-00: DOS VM core — lifecycle, private disk, virtual BIOS, CIUKIDOS boot

- **Step:** F3 (first directive). **Contracts:** `dos-dpmi-contract.md`
  (VM lifecycle and containment; V86 monitor and BIOS boundary; acceptance
  row "F3 lifecycle"; reuse table), `boot-memory.md` (per-VM budget:
  resident cap 32 MiB, ≤ 8 MiB low memory, address space `0–BFFFFFFF`,
  conventional memory never on physical low memory), `execution-abi.md`
  (V86 contexts beside native tasks, guarded stacks, generation-safe
  entry), `vfs-storage-contract.md` (private image overlays),
  `device-firmware-ownership.md` (per-VM virtual PIC/PIT, no physical
  passthrough), `test-architecture.md`, `foundations-transition.md`
  (CIUKIDOS runs unchanged on a private FAT16 disk with virtual firmware;
  FAT16 sector routines of the 0.8 boot files may be reused).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-vm-core`, from `main` at or after `266bbf6`.
  Files: new `src/kernel/vm/dosvm.c`, `dosvm_disk.c`, `dosvm_bios.c`,
  `src/kernel/include/ciuki/dosvm.h` (the kernel-internal API the other
  F3 directives build on: create/destroy, memory access helpers with
  bounds, per-VM I/O port handler registration, IRQ raise, timer hooks,
  text-cell buffer access); `src/kernel/vm/v86.c` only where the monitor
  must grow (complete prefix and operand/address-size decode, INS/OUTS
  bounded and restartable, HLT, segment-limit checks) with
  `tests/host/v86_test.c` extended; `src/kernel/probes/f3_probes_vm.c`
  registered through `scripts/build_kernel.py`; new
  `scripts/build_dos_image.py` and `scripts/build_image.py` (payload
  `\SYSTEM\DOS\DOSBOOT.IMG` with manifest hashes); `tests/suites/f3-vm.json`;
  host tests; new `docs/design/f3-acceptance.md` (selector grammar
  `f3:<probe> run=<8hex>`, records, the rows this directive delivers);
  an appendix "Personality changes recorded" in `dos-dpmi-contract.md`
  (facts only, no requirement changes).

## Scope

1. **VM object and lifecycle.** States creating → booting →
   runnable/blocked → stopping → dead with a generation identifier on
   asynchronous work; one idempotent cleanup path for shell exit,
   explicit termination and unhandled guest faults; failed creation
   unwinds every allocation. Each VM owns a private address space (guest
   linear `0–0x0010FFEF`; kernel mappings shared, supervisor-only,
   CR0.WP; page zero is the VM's IVT), zeroed private memory from the
   NORMAL zone, a V86 task on a guarded kernel stack, software virtual IF
   only (CR4.VME/PVI stay off), IOPL=0 with a deny-all I/O bitmap, its own
   virtual PIC and PIT (the models already in `v86.c`, made per VM), HLT
   blocks the VM, guest CLI never blocks host preemption, guest fault →
   VM termination with a record while another VM and the native heartbeat
   keep running. Admission per `boot-memory.md` with the arithmetic in a
   record; refusal is explicit.
2. **Private disk and virtual BIOS.** `scripts/build_dos_image.py` builds
   deterministically a FAT16 boot image `DOSBOOT.IMG` (boot sector,
   `CIUKIDOS.SYS` assembled from `src/boot/floppy_stage1.asm` with the
   recipe used by `make legacy-build-full`, the DOS shell of point 3,
   `AUTOEXEC.BAT`, the DOS-side probes of point 4) and `build_image.py`
   places it at `\SYSTEM\DOS\DOSBOOT.IMG` in the manifest. This is not the
   floppy profile: it is a VM payload. The VM opens it through the VFS
   read-only with a private copy-on-write overlay (in VM memory or in a
   scratch file under `\SYSTEM\DOS\`; choose and record; unwound at
   cleanup; forced termination reports possible unflushed private data).
   Virtual INT 13h (CHS and EDD, geometry/status, validated bounded
   buffers, unknown drives fail), INT 11h/12h/15h (synthetic equipment and
   memory, VM A20, scheduler waits; never host E820), INT 16h/10h at the
   minimum for a text shell (keyboard queue from the existing `fwinput`
   path; text mode 3 into a kernel-side 80×25 cell buffer readable by the
   probe; full VGA is f3-02), INT 1Ah from VM time, INT 19h, reset ports
   and keyboard-controller reset → VM-local reset/termination. Guest ROM
   stubs resolve to these services; no physical firmware is executed.
   Unknown ports follow the documented absent-device response.
3. **DOS shell.** Use the smallest program already in the tree that shows
   a prompt, runs a `.COM`/`.EXE` by name (INT 21h AH=4Bh) and exits
   (inspect `src/com/`; the FreeDOS sources under `third_party/freedos/`
   are local-only and are not used). If none fits, write
   `CIUKICMD.COM` in NASM: prompt, `DIR`, `TYPE`, run program, `EXIT`.
   Record the choice in the design note.
4. **Record channel.** A documented guest I/O port (one byte at a time,
   bounded line length, rate-limited, VM-tagged) lets DOS-side payloads
   emit `CIUKI_TEST` records through the kernel's `rec_emit`; port the
   payloads of `src/probes/doscompat/` to it and add `DOSREADY.COM` run
   from `AUTOEXEC.BAT` (DOS version, free conventional memory, drive
   letter) so a boot is observable without a console.
5. **Probe `f3:vm-boot`.** Create a VM, boot `DOSBOOT.IMG` to the shell
   prompt (observed through `DOSREADY.COM` and the text-cell buffer),
   then 100 create/boot/exit cycles and 100 create/fault cycles (a
   payload executing a privileged instruction and one accessing an
   invalid port), with page/handle/device baselines restored, IVT and
   disk sentinels never crossing VMs, native heartbeat progress; records
   per the contract row "F3 lifecycle". Suite `f3-vm.json` on
   `qemu-t23`, `qemu-e500`, `qemu-min128` with the F0–F2 prerequisites;
   the probe joins the sweep tables as the F3 phase.
6. **Host tests:** lifecycle state machine with injected failures, INT 13h
   bounds, overlay semantics, admission arithmetic, monitor decode
   additions under `v86_test.c`, record channel framing.

## Out of scope (other F3 directives)

A20/XMS/UMB/EMS (f3-03), keyboard/mouse/PIC/PIT device models beyond the
shell minimum (f3-01), VGA modes and the desktop window (f3-02), DPMI
(f3-04), SB16/OPL (f3-05), workloads (f3-06), shared volumes (F4).

## Acceptance by the lead

Host tests; kernel and image build with `DOSBOOT.IMG` in the manifest;
QEMU `f3-vm` `vm-boot` PASS on the three profiles with the prompt reached
and the 100+100 cycles; `f2-all` unaffected.
