# F6 kernel PE loader and callback prerequisite

Implementation scope: f6-04, under ND3/ND6/ND12 of
[the binding contract](ndis-miniport-host-contract.md). This document does not
qualify a binary, PCI revision, budget profile, or foreign execution boundary.

Research and comparison

- Microsoft's [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
  defines i386 PE32, native subsystem, section/directory extents, imports and
  base relocations. `sdk/pe/pe.c` extracts the existing F5 mechanism, including
  HIGH/LOW/HIGHADJ and 32-bit modular relocation arithmetic. Address/target
  extents are checked; numerical wrap in a HIGHLOW value is PE32 arithmetic.
  F5 retains its accepted unusual layouts; kernel policy refuses truncated
  raw data, overlapping raw section extents, TLS, delay imports, non-native images and entries outside executable
  sections. Bound imports use names again; a bound IAT without names fails.
- Microsoft's NDIS5 [NdisMRegisterMiniport](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553602(v=vs.85))
  and [MiniportInitialize](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff550472(v=vs.85))
  distinguish registration from starting an adapter. This step implements
  neither miniport registration nor a fake successful DriverEntry. The shim
  table reserves explicit f6-05/f6-06 contract symbols, all unimplemented.
- Microsoft's [hardware priorities](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/managing-hardware-priorities)
  and [stdcall](https://learn.microsoft.com/en-us/cpp/cpp/stdcall?view=msvc-170)/[fastcall](https://learn.microsoft.com/en-us/cpp/cpp/fastcall?view=msvc-170)
  documentation inform the explicit callback IRQL model and ABI metadata.
  Fastcall stack-byte counts exclude ECX/EDX parameters. Cdecl stack bytes are
  zero for variable-arity/caller-cleaned metadata; no symbol decoration is
  guessed during lookup. The five-argument
  [NdisMAllocateSharedMemory](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismallocatesharedmemory)
  reservation is stdcall with 20 stack bytes.
- [MmGetSystemRoutineAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmgetsystemroutineaddress)
  searches kernel/HAL functions, not arbitrary driver exports. Counted UTF-16
  lookups and [NdisGetRoutineAddress](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndisgetroutineaddress)
  share the import table and return zero for
  unknown/unimplemented routines. Their future PE-visible shims must validate
  the 32-bit UNICODE_STRING before calling these internal helpers.
- Local Windows 98 Resource Kit `build/rskt/wrkc16_1.htm` describes protected-mode
  NDIS3.1 network drivers. It does not supply an NT5 miniport ABI. NT5 PE policy
  follows the Microsoft NDIS5 pages above; Win9x support is not inferred.

Mechanism and allocation

The source library has consumer admission, diagnostics, writable-page and
allocator callbacks. Small source adapters compile that same implementation
into every existing SDK core recipe and the kernel's new `ndis` source group.
F5 retains resource/TLS/protection policy and uses the shared import visitor
for name/ordinal/OFT parsing. The pre-existing kernel Win32 parser remains a
separate streaming recognition/bootstrap step; it does not load drivers and
ntdll validates its seed again with the shared mechanism.

The kernel resident backend allocates owned contiguous PMM pages, attributes
them to MM_NET, pins them with the existing ledger, verifies supervisor PTEs
and retains them for module lifetime. This uses the existing supervisor direct
map; i686 supplies no NX guarantee. No process/user mapping is created. A
caller must supply an already reserved share of the common F6 4 MiB budget;
the loader cannot authorize an additional budget. A relocation-free image
loads only if its preferred VA is the allocated VA. Failures before entry
release the private image/pin. If the ledger refuses unpin, the resident
allocator retains its arena and blocks another allocation; it never frees an
uncertain pin. A quarantined or entered image cannot unload
without a future reviewed drain/idle lifecycle. No memory limit is changed.

The production table has no guessed ordinal mappings: ordinal zero denotes
unassigned, never a resolvable ordinal. A lead-reviewed NT5 module/version
mapping is required before any ordinal can be implemented. Synthetic test
entries use explicit ordinals and function/data addresses; production loads
always use the shipped table. Shim implementation is f6-05/f6-06 work.

Executable boundary: closed admission gate

Repository inspection found kernel exceptions still reach `panic_frame`
in `src/kernel/arch/trap.c`; `copy_user`/expected-fault fixups are narrowly
scoped and cannot unwind an arbitrary driver callback. The display watchdog
is scheduled work and cannot independently preempt a ring-0 CLI/spin. Native
`netif_quarantine` takes a mutex and may call native driver methods; it is not
an emergency foreign-code fence. Reusing these paths as if they recovered a
foreign fault would violate ND12. No trap/panic path is added or repurposed.

Consequently **production foreign execution is disabled unconditionally** in
`ntimage_enter`, even for an import-free driver. It records the prerequisite
refusal and rolls back a pre-entry image. There is no run-time flag to enable
it. The two-argument callback thunk exists only in the host-model build.

The callback state model records entry/exit, validates executable addresses,
claim generations, bounded nesting and IRQL transitions; waits above APC_LEVEL
quarantine. ISR nesting is permitted only at the configured DIRQL. On fault
or independent watchdog expiry it marks the image permanently quarantined,
invokes an independently provided host fence, and uses existing
`registry_quarantine` on exact generation claims. Even an idle proof does not
permit image release in this step. Unproved fence results retain every pin.
A model watchdog is an externally delivered event while the callback is
active; polling after return is not nonreturn detection or runtime evidence.

Before opening the gate the lead must cross-review and demonstrate:

1. An exception boundary that abandons the foreign stack and never resumes
   faulting/corrupted code, with an honest controlled-stop case for host-memory
   corruption or a fault in a host critical section.
2. A watchdog/fence reachable despite a nonreturning callback or CLI, preserving
   peer IRQs, and a qualified independent stop sequence for each PCI revision.
3. Generation invalidation, submission cancellation, interrupt/DPC drain and
   engine-idle proof before reclaim; keep image/DMA/callback storage if unsure.
4. The synthetic exception/nonreturn fixtures in the permitted runtime tier,
   and exact hardware/hash/profile evidence. Host models alone do not close
   these gates.

No owner-pack payload or external implementation code is used by this step.

The staged, incomplete f6-05 recovery mechanisms and their host-test limits
are documented in [ndis-containment.md](ndis-containment.md). They do not open
the production gate described here or qualify the three pinned profiles.
