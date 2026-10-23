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
