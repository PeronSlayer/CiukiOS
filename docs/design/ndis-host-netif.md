# NDIS 5 host and network adapter

This implements f6-06 on the ND14 user boundary. Exact-profile qualification
remains in `src/kernel/ndis/profiles.c`; no acceptance record is added here.

Primary sources and repository decisions

- Microsoft's [NDIS 5 registration](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553602(v=vs.85))
  and [initialization](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff550472(v=vs.85))
  separate DriverEntry registration from each adapter start. Copy the versioned
  characteristics, validate executable callbacks, then offer Ethernet at index 0.
- [Attributes](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsetattributesex)
  precede DMA/IRQ admission. Zero hang interval selects two seconds; intervals
  are rounded to the documented two-second granularity.
- [SendPackets](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff550524(v=vs.85))
  and [receive indication](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553533(v=vs.85))
  require distinct serialized/deserialized packet ownership. Keep pending
  descriptors until completion; copy RX before returning a descriptor.
- [Interrupt registration](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553596(v=vs.85))
  uses the already claimed raw IRQ and existing native deferred dispatcher.
  Shared PCI requires an ISR and qualified per-function masking.
- [NdisWaitEvent](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndiswaitevent)
  uses milliseconds, with zero meaning indefinite. The existing synchronous
  `nt_wait` cannot service that same host thread's user DPC. Save the syscall
  return frame in kernel storage, leave its outer deadline active, allow ISR
  and DPC callbacks on separate stacks, then restore the saved frame with the
  actual result. No native stack remains blocked on its own user callback.
- [KeWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitformultipleobjects)
  distinguishes WaitAll, WaitAny indices, NT relative/absolute times and a zero
  polling timeout. Consume synchronization events/semaphores atomically only
  when the complete wait condition is satisfied. Alertable/UserMode waits and
  unimplemented object kinds refuse rather than manufacture an APC or success.
- The [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
  STI instruction delays interrupt recognition through the next instruction
  only when enabling IF. Use TF for a native instruction, and retire trapped
  HLT or patched PUSHF/POPF/IRET explicitly. Keep physical IF set, hide the
  internal TF in saved flags, and do not extend the shadow for STI;STI.
- The upstream [ReactOS DDK header](https://github.com/reactos/reactos/blob/master/sdk/include/ddk/ndis.h)
  (public-domain definitions) supplies an independent macro/structure check:
  fixed-width i386 characteristics, MDLs, packet-private and OOB records, and
  the miniport block's macro callback slots. No implementation is vendored.
- Local Resource Kit `build/rskt/wrkc16_1.htm` concerns protected-mode Win9x
  networking/NDIS 3.1. It does not define the NT5 ABI used by these profiles.

The kernel owns descriptors, completion accounting and DMA ledger references.
Process-local x86 veneers cross scalar validated services. No kernel address
is callable through an IAT or macro slot. The kernel network adapter consumes
the validated ND14.1 ring into its existing private frame storage. This
boundary contains CPU faults, not arbitrary device DMA.

## Implementation decisions (f6-06 completion)

- **Export union.** The NDIS import union of the three pinned profiles is 75
  symbols (plus the macro-reached `NdisM*` completion/indication slots). All are
  implemented except four, which are deliberate explicit refusals (no success
  stub; the loader fails such a resolution closed), recorded for a reviewed
  follow-up:
  - `NdisMRegisterDevice`/`NdisMDeregisterDevice` (imported by `e1000325.sys`
    and `e100b325.sys`) create a stand-alone control device object with an
    `IRP_MJ_*` dispatch table. Per
    [NdisMRegisterDevice](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismregisterdevice)
    this is a full user-facing IOCTL control path, which ND7 flags as
    additional work rather than a harmless stub; CiukiOS routes no IOCTLs to
    such a device. Left disabled pending a reviewed restricted upper-edge path.
  - `NdisMSetMiniportSecondary`/`NdisMPromoteMiniport` (LBFO; imported by
    `atnicm51.sys` and `e1000325.sys`) are the load-balancing/failover bundle
    API. Per
    [NdisMSetMiniportSecondary](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553629(v=vs.85))
    and [NdisMPromoteMiniport](https://learn.microsoft.com/en-us/previous-versions/windows/hardware/network/ff553566(v=vs.85))
    a single-adapter host has no bundle; a driver calls `SetMiniportSecondary`
    only after matching a registry `BundleIdentifier` against an already
    initialized sibling, which cannot occur here. They remain refused pending a
    lead decision rather than returning a never-exercised `NDIS_STATUS_FAILURE`.

  Consequently none of the three pinned images resolves its complete import set
  yet, so none can load even if a qualification record existed. Qualification is
  lead-owned and `profiles.c` stays empty; opening a profile requires both the
  lead's runtime evidence and a decision on these four exports.
- **STI one-instruction shadow.** An emulated STI that transitions virtual
  IF 0->1 arms `sti_shadow` and sets the architectural TF so the next
  instruction single-steps. `ndis_host_take` refuses every callback while the
  shadow is armed, so a pending virtual interrupt is delivered only after that
  one instruction retires. A natively executed instruction retires on its #DB
  (`ndis_host_debug`); a trapped/emulated instruction (CLI/STI/HLT #GP, an
  INT3-patched PUSHF/POPF/IRET, or the host INT 80h syscall) retires through
  `ndis_host_retired`. The internal TF is hidden from the driver's saved flags,
  a guest-owned TF still faults, and `STI;STI` does not re-arm. Covered by
  sti/hlt, sti/ret and sti/INT3-PUSHFD host cases and by actual i386 CPL3
  single-step in the user-process fixture.
- **Asynchronous wait bridge.** `NdisWaitEvent`, `KeWaitForSingleObject`,
  `KeWaitForMultipleObjects` and `NdisMSleep` inside a foreign callback do not
  block the host thread. `ndis_nt_wait_begin` evaluates the predicate once;
  if unsatisfied it returns `STATUS_PENDING`, the syscall frame is parked
  (`ndis_nt_wait_park`), and the kernel return boundary keeps delivering ISR/DPC
  work while the callback is suspended. On signal or timeout `poll_waits` marks
  the wait ready and `ndis_nt_wait_resume` restores the parked frame with the
  real NDIS/NTSTATUS result; no success or timeout is fabricated. The kernel
  watchdog still enforces the outer callback budget, so an indefinite wait
  quarantines rather than stalling. Synchronous `nt_wait` still refuses with
  `user-wait-continuation-required` for any path that is not routed through this
  bridge. WaitAll/WaitAny, relative/absolute 100 ns timeouts, a zero polling
  timeout, notification vs synchronization events and semaphore decrement are
  honoured ([KeWaitForMultipleObjects](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitformultipleobjects)).
- **netif adapter.** `net_ndis` publishes through `netif_publish_external`,
  which marks the common `struct netif` as externally owned: the kernel network
  worker keeps the copied RX/TX queues and the public netctl state record, while
  the host process owns the registry/fence lifecycle. `stop`/`quarantine` defer
  to the host's independent fence (`EAGAIN` until `MiniportHalt` completes, then
  a fenced idle proof) and never free borrowed BAR/DMA backing. Carrier/link
  state feeds f2-100 netctl.
- **Test harness note.** `tests/host/ndis/user_process_test.c` restores the
  emulated user ESP through `REG_ESP` (Linux i386 restores the user stack
  pointer from `sigcontext.esp`, not `esp_at_signal`/`REG_UESP`), so an emulated
  PUSHF/POPF stack adjustment takes effect on `sigreturn`. This is a host
  ucontext portability fix in the fixture only; the production emulation is
  unchanged.
