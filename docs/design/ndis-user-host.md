# ND14 user host implementation boundary

This implements part of f6-05 Amendment 1. It does not qualify a miniport.
`ndis_user_host_available`, `ndis_execution_available`, task attachment and
production initialization remain unconditionally closed. There is no boot
argument or user syscall that enables them. Part 2 must wait for part 1.

Research and repository comparison

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  Volumes 2 (CLI, STI, HLT, PUSHF, POPF, IRET) and 3A (I/O bitmap, paging,
  cache aliases): CPL3/IOPL0 traps CLI/STI/HLT; POPF does not virtualize IF.
  The old VGA switch changed only six bitmap bytes. The scheduler now resets
  the complete bitmap before installing the incoming VGA/NDIS grant, including
  on first activation. The FF sentinel remains denied. Host grants are private
  kernel records, conditioned on an active ownership generation; ordinary
  processes cannot inherit them. Large port allowlists still need timing
  qualification before admission.
- The explicit `NDIS_USER_FLAGS`/`RESTORE_FLAGS` scalar syscall protocol reads
  and restores virtual IF only. It is a shim protocol, not binary rewriting
  or transparent emulation of arbitrary flag instructions. Exact unprefixed
  CLI/STI/HLT #GP instructions are supported; other encodings fault the host.
  Physical IF stays set and IOPL stays zero on user return. A separately
  reviewed mechanism for dependent binary flag-save patterns is still needed.
  The executable fixture tests physical and virtual IF separately.
- [Microsoft PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  relocation uses execution VAs, not allocator pointers. The shared mechanism
  already supports distinct staging pointer/base; the old NDIS policy required
  kernel VAs. `ntimage` now has an explicit user policy and a pinned user
  staging allocator. Imports require process-local, pinned mappings and never
  accept a kernel export address. Production exports remain unimplemented.
  The instruction scan is diagnostic only, including for the old resident
  loader. Neither scan success nor CLI refusal establishes containment.
- [Upstream VFIO INTx source](https://raw.githubusercontent.com/torvalds/linux/master/drivers/vfio/pci/vfio_pci_intrs.c)
  checks/masks the function before notification and requires an exclusive
  interrupt without per-function DisINTx. Host admission requires a qualified
  masker for shared lines. Native mask/recheck operations must be supplied by
  a reviewed exact revision profile; none is qualified here. The existing
  synchronous registry Boolean cannot represent pending, so a separate
  generation/token-tagged pending publication and resolution API was added.
  Pending passes do not count as not-mine or advance the stuck-line counter.
  The dispatcher retains sole EOI ownership. Asynchronous not-mine results
  have a separate counter; rearm rechecks native cause before and after unmask.
- [VFIO documentation](https://docs.kernel.org/driver-api/vfio.html) explains
  why DMA needs an IOMMU. These i686 CPU mappings do not restrict device DMA.
  The existing PCI command fence is retained, with no D3 reset inferred from
  a PM capability. An unproved idle result retains backing for the boot.
  A driver properties UI must say this when f6-07 exposes admission.
- [Microsoft DPC rules](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/introduction-to-dpc-objects)
  and [spin locks](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keacquirespinlock)
  preserve deferred work and actual exclusion. The user-host queue serializes
  ordinary callbacks and allows only a higher ISR to nest. Virtual IF and an
  interrupt synchronization hold block ISR delivery, never the physical PIT.
  ND14 kill thresholds are ISR 5 ms, DPC 50 ms, passive 500 ms, lifecycle 10 s.
  Deadlines start at queue publication, include blocked/yielded time, and nested
  callbacks cannot extend an outer deadline. The existing 2 ms worker bound
  remains a dispatch/batch target; 50 ms is a kill threshold, not permission
  for a native worker to monopolize the CPU. The kernel top half still has
  its independent 50 us target and requires measured qualification.

The kernel user-return boundary delivers callbacks to a pinned process-local
dispatcher on separate resident callback stacks. It saves the interrupted
user context in kernel metadata, so an ISR can preempt an ordinary callback
without discarding the outer deadline. Completion accepts only the top
generation/token and restores that saved context, never a user-supplied frame.
An idle host parks in the kernel until trusted work notification; reporting
completion cannot grant an unbudgeted return to the abandoned driver stack.
TAKE is reserved and refused: the host cannot admit its own callbacks.
The final NDIS callback registration and process-local dispatcher/shim payload
still need integration with f6-06 and part 2 after part 1's prerequisites pass.

Memory and teardown decisions

`as_map_borrowed` is restricted to raw device address spaces with no native
uaddr owner. It preserves PCD/PWT and marks aliases as borrowed. Ordinary
mapping/replacement cannot overwrite or convert these PTEs. Revoke and address
space destruction discard aliases and table ownership without freeing BAR or
DMA backing. These APIs are not exposed to ordinary mmap/fork/exec.

BAR grants require whole-page active claims without page rounding. Passive
grant admission checks all supervisor aliases and refuses conflicting cache
attributes. DMA grants use exact live F6 ledger tokens, not a PFN-only test;
WB RAM retains the direct map's cache type. A UC alias of WB RAM is refused.
Allocations come from the existing reserved F6 budget; no cap is increased.
Exact BDF/BAR extents, source masks and emergency stops still require a pinned
qualification profile before any grant can be used by foreign code.

Stop publishes FENCING and invalidates the generation before any teardown;
it masks, invokes the independent fence, detaches deliveries, transfers backing
to boot quarantine, revokes aliases, records the fault, then cancels the task.
Ordinary task exit/kill invokes the host stop hook first. The return boundary
prevents resumption and bypasses application signals. All new host syscalls
are scalar, bounded and nonblocking; native process/file/memory syscalls are
not forwarded. Kernel continuation cancellation is bounded to the next return
of these calls. A stuck native continuation is diagnosed after one tick and
retained, never forcibly reaped with live kernel ownership.

The watchdog runs after timer EOI over at most the existing two adapter slots,
including noncurrent tasks. Its nominal check interval is 1 ms. The nominal
maximum detection delay is one interval plus measured native IRQ-off/firmware
latency; no finite wall-clock bound against arbitrary SMM or kernel stalls is
claimed. Runtime timing evidence remains required.

Shared packet publications

`packet_ring` uses fixed pinned slots, bounded offsets and a kernel-private
cursor/token/ownership table. Acquire loads observe the process release
publication; producers must write descriptor and payload before publishing
the incremented cursor with a release store. The consumer snapshots each word
once, checks producer distance (including wrap), generation, slot, fixed
offset, length and live token, then copies into kernel-owned storage before
completion. Malformed snapshots are errors to quarantine at the adapter seam.
Driver writes during a copy can change packet content, never kernel bounds.

Slot reservation returns backpressure when all sixteen copied slots are owned.
Completion detaches ownership before a possibly reentrant callback, and token
wrap is refused. Notifications are hints: both sides drain their bounded
cursor state after every doorbell and before sleeping; the native wait/doorbell
integration must atomically check pending state when f6-06 binds the netif.
Teardown invalidates the generation and errors every outstanding token once.
The future netif adapter must connect quarantine/interface-down to these APIs;
this directive does not implement f6-06's NDIS registration/data path.

Evidence and unclosed prerequisites

Host fixtures are authored synthetic code, never owner-pack inputs. They test
state machines, real production mapping loops and PE policies. The separate
i386 fixture attempts actual ring-3 CLI/STI/HLT and forbidden I/O plus faults
and virtual flag handling. Host signal handling cannot prove Ciuki TSS loads,
PIC sharing or real device stop. Required lead runtime evidence and the reviewed
binary flag mechanism remain prerequisites. See `build/codex/f6-05-report.md`
for exact commands, failures, skips and remaining implementation work.
