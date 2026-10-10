# Directive f3-03: A20, HMA, XMS 3.0, UMB and EMS 4.0 for DOS VMs

- **Step:** F3. **Contracts:** `dos-dpmi-contract.md` ("Memory services
  and admission at 128 MiB": A20 wrap/on, HMA ownership, every XMS call,
  UMB boundaries, EMS 4.0, quota exhaustion and failed resize preserve
  data, admission arithmetic logged, explicit second-VM refusal;
  acceptance row "F3 memory"), `boot-memory.md` (per-VM budget: ≤ 24 MiB
  XMS/EMS/DPMI pool, ≤ 8 MiB low memory, 32 MiB resident cap, headroom),
  directive f3-00 (`dosvm_set_a20`, memory helpers, admission ledger).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-vm-memory`, from `main` after f3-00 is merged.
  Files: new `src/kernel/vm/dosvm_xms.c`, `dosvm_ems.c`, `dosvm_umb.c`;
  the guest-visible driver entry points (an XMS driver stub resident in the
  VM's HMA/UMB area whose far-call entry traps to the kernel through the
  f3-00 BIOS boundary; EMS `INT 67h` with the page frame at `E000h` or
  `D000h` per configuration; UMB arena published through the XMS UMB
  functions and DOS `INT 21h/58h` strategy stays personality-owned),
  `dosvm_bios.c` INT 15h `87h/88h/E801h/E820h` synthetic values and
  `24h` A20 gate; port `92h` and keyboard-controller A20 commands;
  `src/kernel/probes/f3_probes_memory.c`; DOS-side payloads under
  `src/probes/dos/` (record port) exercising every XMS and EMS function;
  `tests/suites/f3-vm.json` additions; host tests;
  `docs/design/f3-acceptance.md` (`## f3-03`).

## Scope

1. **A20 and HMA.** Guest A20 state via port `92h`, 8042 output port
   and INT 15h `2400h–2403h`; wrap-around at 1 MiB when off (alias
   mapping of the first 64 KiB − 16 bytes); HMA (`FFFF:0010–FFFF:FFFF`)
   allocated to one owner through XMS `01h/02h`; `dosvm_set_a20` of
   f3-00 is the only switch.
2. **XMS 3.0.** Functions `00h–12h` and the super-extended `88h–8Fh`
   (`XMS 3.0` spec of the contract): query, HMA, A20, allocate/free
   EMB, move (bounded, validated both sides, 32-bit lengths, overlapping
   moves as the spec), lock/unlock returning a 32-bit physical address
   inside the VM's pool (the pool is private VM memory above 1 MiB,
   contiguous within the VM's address space; DPMI later shares it),
   get/set handle info, resize preserving data, UMB allocate/release
   (`10h/11h`), 64 KiB granularity where the spec says. Errors per spec
   (`80h`, `81h`, `90h`–`B2h`). Quota: the per-VM pool from the admission
   ledger; exhaustion returns `A0h` and leaves every block intact.
3. **UMB.** Page-granular arena between the end of the VGA aperture and
   the ROM stubs (`C800h`–`EFFFh` minus the EMS frame), published only when
   configured, with sizes the DOS personality can accept (retain existing
   CIUKIDOS UMB behaviour, recorded).
4. **EMS 4.0.** `INT 67h` functions `40h–5Dh` as the LIM spec of the
   contract: status, page frame address, pages, allocate/map/unmap/
   release, version, save/restore mapping, handle names, get/set page
   map, move/exchange memory regions (bounded, validated), realloc; the
   page frame is four 16 KiB physical windows remapped by page tables in
   the VM's address space (no copying); errors per spec.
5. **Probe `f3:vm-memory`**: DOS-side payloads run every function with
   expected results and data-integrity patterns across resize/move/
   exhaustion; A20 wrap test; HMA ownership; two VMs: the second is
   admitted or refused explicitly with the arithmetic recorded (the
   128 MiB profile `qemu-min128` must refuse or admit per the ledger);
   records per the "F3 memory" acceptance row.
6. Host tests: the XMS/EMS state machines and the move/resize validators
   against a fake VM memory.

## Acceptance by the lead

Host tests; kernel build; QEMU `f3-vm` memory cases PASS on t23/e500/
min128 with the admission arithmetic recorded; `vm-boot` unchanged;
`f2-all` unaffected.
