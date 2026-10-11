# Directive f3-04: kernel-owned DPMI 0.9 host for DOS VMs

- **Step:** F3 (hardest directive). **Contracts:** `dos-dpmi-contract.md`
  ("Kernel-owned DPMI 0.9": discovery, entry, per-VM LDT and tables, the
  complete INT 31h table with its refusals, failure conventions, mode
  transitions and stacks, virtual IF sharing, termination cleanup,
  virtual IRQ and exception dispatch, DOS/4GW selecting Ciuki VMM;
  acceptance row "F3 DPMI"), `execution-abi.md` (V86/DPMI contexts beside
  native tasks, LDT/TSS/IDT dispatch, guarded stacks, FPU ownership,
  generation-safe entry; INT 31h path distinct from native INT 80h),
  `boot-memory.md` (per-VM pool ≤ 24 MiB for XMS/EMS/DPMI, address space
  `0x00110000–0xBFFFFFFF`), directives f3-00 (VM core), f3-01 (PIC, IF
  shadow), f3-03 (XMS pool shared with DPMI allocations).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh` (Astra only on the
  owner's request or after two Sol failures on the same problem).
- **Worktree:** `wt/f3-dpmi`, from `main` after f3-00, f3-01 and f3-03
  are merged. Files: new `src/kernel/vm/dpmi.c`, `dpmi_desc.c`,
  `dpmi_switch.c`, `dpmi_dispatch.c`, `src/kernel/include/ciuki/dpmi.h`;
  `src/kernel/arch/desc.c` and `entry.asm`/`isr.asm` only for the per-VM
  LDT load and the INT 31h / client-ring-3 entry path (never the native
  INT 80h path); the guest-visible `INT 2Fh/1687h` entry stub in the VM's
  ROM area through f3-00's BIOS boundary; `src/kernel/probes/f3_probes_dpmi.c`;
  DOS-side 16-bit and 32-bit client payloads under `src/probes/dos/`
  (NASM, record port) covering every table row; the pinned DOS/4GW probe
  of `src/probes/vm/dpmi_*` rebuilt with the existing OpenWatcom recipe
  (`scripts/build_pmirqsb_dos4gw.sh` as the model) with its extender and
  data hashes recorded; `tests/suites/f3-vm.json` additions; host tests
  (descriptor and state machines against a fake VM); `docs/design/
  f3-acceptance.md` (`## f3-04`); `execution-abi.md` amendment for the
  client entry path.

## Scope, in stages (each stage host-tested before the next)

1. **Discovery and entry.** `INT 2Fh/1687h` only when the whole contract
   is implemented (until then it stays unadvertised: a build-time list or
   a per-VM flag set by the probe for staged testing, recorded); `1686h`
   mode report; entry for 16- and 32-bit clients with the specified initial
   selectors and registers, failure without a partial transition; clients
   at ring 3, IOPL=0, denied I/O; per-VM kernel-managed LDT (up to the
   contract's selector limits), 0.9 tables shared by the VM's clients, the
   first client fixing the 16/32-bit convention, opposite-width nested
   entry failing cleanly; selector ownership tracked per client; aliases
   `0002h` VM-owned; no privileged descriptors, call gates or kernel
   mappings reachable; flat descriptors may span 4 GiB with page
   permissions as the protection boundary.
2. **Descriptor, memory and vector services.** Rows `0000–000D`,
   `0100–0102` (DOS arena through the personality with matching
   selectors), `0200–0205`, `0400`, `0500–0503` (quota from the per-VM
   pool shared with XMS of f3-03), `0600–0604`, `0702/0703`, `0800`
   refused with CF, `0900–0902` sharing the VM's virtual IF with the
   monitor, `0A00`, `0B00–0B03` watchpoints switched with VM state;
   `0004/0005/0700/0701` reserved fail; the 1.0-only list fails
   unsupported. Failures set CF, preserve unspecified registers and use
   the 0.9 outputs; unknown functions mutate nothing.
3. **Mode transitions and stacks.** `0300–0302` with the VM's vectors and
   validated register/stack buffers; `0303–0306` callbacks (≥ 16 slots per
   client) and raw switches with guarded resident transition stacks; eight
   tested nested transitions; reentry preserving continuations;
   exhaustion failing or terminating only the VM; no VFS/allocator lock
   survives guest execution; IRET-return callbacks, far-return exception
   frames, guest-edited frames validated.
4. **Dispatch.** Virtual IRQs to the active client's protected handler or
   the default V86 reflection; unhandled software interrupts to the VM's
   real vectors; exceptions through host fault resolution first, then the
   client's validated locked-stack handler or VM termination; guest IRQ
   vectors distinguished from CPU exceptions; reflection preserving
   registers and the virtual PIC's in-service/EOI rules; protected-mode
   CLI/STI and `090xh` share virtual IF; PUSHF/POPF caveats of the
   contract respected; guest IF=0 never acquires physical PIC ownership
   nor is forced on by a timeout.
5. **Termination.** V86 and protected `INT 21h/4Ch` reach one cleanup
   path freeing allocations, callbacks, locks and selectors while the
   parent survives; stale handlers into freed storage terminate the VM;
   fault termination unwinds all nesting without guest help.
6. **Clients and evidence.** Probe `f3:dpmi`: the 16- and 32-bit NASM
   clients exercise every row (success and invalid inputs), nesting,
   exceptions, stale selectors, watchpoints, cleanup, refused 1.0/VCPI/
   map requests changing nothing; then the pinned DOS/4GW probe selects
   Ciuki VMM as host (no HDPMI/VCPI), runs its interrupt/port/lifetime
   tests and exits; records per the "F3 DPMI" acceptance row with
   executable/extender hashes. Cases on t23/e500/min128.

## Acceptance by the lead

Host tests; kernel build; QEMU `f3-vm` DPMI cases PASS on the three
profiles; `vm-boot`, devices and memory cases unchanged; `f2-all`
unaffected; `INT 2Fh/1687h` advertised only once every row passes.
