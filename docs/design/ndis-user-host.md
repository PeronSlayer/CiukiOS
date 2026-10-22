# ND14 user host implementation boundary

Amendment 2 implementation decisions

The [Intel instruction reference](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
defines INT3's saved EIP as the address after the breakpoint. The loader therefore
records the opcode RVA, operand width and original opcode in kernel storage and
replaces only that byte. A single operand-size prefix is preserved. PUSHF saves
virtual IF; POPF and same-privilege IRET restore it while physical IF stays set
and IOPL stays zero. Stack accesses use checked, resident user copies. Unknown
breakpoints, changed site bytes, unsupported prefixes and privilege-changing
IRET frames stop the adapter. The table binds to the source image SHA-256.

The [PE relocation specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-reloc-section-image-only)
marks relocation operands, not a complete code/data map. A bounded sorted operand
table excludes those bytes from the linear sweep; immediates and displacements
are consumed by the instruction decoder. Unknown encodings fail patch preparation
closed. This sweep does not prove arbitrary control flow; exact binary runtime
qualification is still mandatory. Executable pages become read-only after patching;
an executable page shared with writable section data is refused rather than
silently exposing writable code or breaking the data section.

The exact part-2 inventory uses `UIU/e1000325.sys`, `UIU/e100b325.sys` and
`UIU/atnicm51.sys` as named by ND13. Same-name `Driver/D/` and `UIU/A1/`
files are different binaries and are not interchangeable qualification inputs.
Microsoft's [spin-lock initialization](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keinitializespinlock),
[IRQL restore](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kelowerirql),
[locked-page mapping](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmaplockedpagesspecifycache),
[counted Unicode append](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlappendunicodestringtostring)
and [file-open](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-zwopenfile)
contracts require owned resident objects, exact restore state and NT statuses.
Process-local NT/HAL veneers must cross the scalar host service; metadata alone
does not implement an export. No kernel address may enter the user IAT.

This implements the f6-05 Amendment 2 mechanisms and native integration. It
qualifies no miniport. `profiles.c` is the sole production qualification
authority and contains no accepted record. Task creation, host initialization
and attachment therefore remain closed by default. A record must bind the
exact image SHA-256, revision/subsystem, measured BAR extents, independent source
mask/cause/stop operations and runtime evidence. Native IRQ registration
currently requires per-function masking; a line-only/exclusive fallback refuses
until registry admission can preserve that exclusivity. There is no user enable switch.
`ntimage_enter` and the older ring-0 execution route remain disabled permanently;
native entry uses a resident ring-3 dispatcher and kernel-owned continuations.
Host-model evidence does not replace the pending permitted runtime evidence.

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
  Physical IF stays set and IOPL stays zero on user return. Amendment 2's
  INT3 site table now supplements that explicit protocol. The executable
  fixture includes patched nesting, callee saves and an IRETD frame, and tests
  physical versus virtual IF separately. Execution is unproved in this sandbox.
- [Microsoft PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format):
  relocation uses execution VAs, not allocator pointers. The shared mechanism
  already supports distinct staging pointer/base; the old NDIS policy required
  kernel VAs. `ntimage` now has an explicit user policy and a pinned user
  staging allocator. Imports require process-local, pinned mappings and never
  accept a kernel export address. The exact 31 NT/HAL function veneers and
  `KeTickCount` data view are process-local; all NDIS exports remain f6-06 work.
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
`native` supplies the position-independent dispatcher, blocked raw task
creation, resident stacks, private callback/result tokens and deferred IRQ
registration. DriverEntry can run before MiniportISR is registered. Its shared
argument vectors contain at most six stdcall arguments. f6-06 supplies NDIS
characteristics, adapter lifecycle and the netif upper edge; none is fabricated
by these lower-edge services.

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
prevents resumption and bypasses application signals. Scalar doorbells are
bounded and nonblocking. NT services copy fixed argument arrays and check each
user buffer; resident wait predicates and calibrated stalls enable physical
interrupts. Waits include cancellation in their predicate. A blocking wait inside
an active foreign callback currently stops with `user-wait-continuation-required`:
it cannot wait for that thread's own user ISR/DPC through a kernel continuation.
The NDIS layer must supply its asynchronous user wait bridge before qualifying
that path; no selected NT export is a wait entry point. Native process,
file and memory syscalls are not forwarded. Cancellation reaches the next
safe service return. A stuck native continuation is diagnosed after one tick and
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
cursor state after every doorbell and before sleeping. `channel` implements
the two directions, RX credits, ordered TX descriptor reuse, minimum frame
padding, generation-checked completions and a kernel-owned notification epoch.
The host WAIT checks the epoch and parks under the same UP exclusion as
publication. Saturation refuses sleeping instead of wrapping into a lost wake.
Teardown invalidates the generation and errors every outstanding token once.
The future netif adapter must connect quarantine/interface-down to these APIs;
this directive does not implement f6-06's NDIS registration/data path.

Evidence and unclosed prerequisites

Host fixtures are authored synthetic code, never owner-pack inputs. They test
state machines, real production mapping loops and PE policies. The separate
i386 fixture attempts actual ring-3 CLI/STI/HLT and forbidden I/O plus faults
and patched virtual flag handling. Host signal handling cannot prove Ciuki
TSS loads, PIC sharing or real device stop. Required lead runtime evidence
remains a prerequisite for every qualification record. See `build/codex/f6-05-report.md`
for exact commands, failures, skips and remaining implementation work.

NT/HAL service boundary

The imported union is 22 NT kernel symbols and 10 HAL symbols. The process-local
veneer page preserves stdcall, register fastcall, cdecl and compiler-helper
stack cleanup; the kernel export addresses remain zero. The tick data page is
read-only and updated with the KSYSTEM_TIME high/low/high publication protocol.
Only the exported union has implemented veneers; unknown ordinals still refuse.

`nt_runtime` supplies nested priority restore records, callback-owned executive
locks, bounded tagged resident pools, owned MDLs/PFNs, deferred DPC insertion and
cancellation, synchronization/notification waits, semaphores and absolute/relative
100 ns timer conversion. Small allocations stay within a page; large ones are
page-aligned. A held lock or unmatched raise cannot escape callback completion.
DMA map-register reservations count worst-case unaligned physical pages against
the 64-register limit, not merely base buffer indices; start validates the original
MDL allocation, splits physical segments by qualified limits and obtains an
actual directional ledger pin. Completion unpins once with an acquire barrier.
Quarantine retains those pins. This is not device-DMA address containment.

[IoCompleteRequest](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iocompleterequest)
requires exactly-once completion without held spin locks. Only upper-edge,
producer-registered IRPs with no lower device/completion chain are supported.
The producer supplies a private resident IoStatus view and byte bound. Completion
snapshots that view, detaches ownership and calls the native requester. Other
IRPs stop the adapter. f6-06 must keep generated requests within this restriction;
no general WDM stack is advertised. Stop cancels registered native requests once.

Counted UTF-16 copy/append, SLIST operations and integer helpers have explicit
bounds and x86 layouts. File opens are restricted to installer-declared immutable
support-file names and read access, with NT error results for missing files.
No system registry or filesystem namespace is exposed. The selected binaries
have no direct NT registry imports; NdisReadConfiguration and the installed
parameter materializer belong to the NDIS/installer layers. PCI reads bind to
the exact owned function; writes cannot touch the common header/command/BAR/W1C
status and require a qualification bitmap for device-specific bytes. The Linux
[configuration-mechanism implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/jic23/iio/+/1da177e4c3f41524e886b7f1b8a0c1fc7321cac2/arch/i386/pci/direct.c)
and the existing native fence use serialized CF8/CFC transactions. Port and MMIO
services validate live claims, widths and owned extents, and perform native-width
accesses. Stalls longer than the existing calibrated 1 ms service refuse; no
requested delay is shortened. These bounds require exact-profile qualification.

Concrete source operations are available for qualification: PCI INTx Disable
and nondestructive Interrupt Status, following the [QEMU PCI implementation](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/pci/pci.c),
and byte-sized 8255x SCB M/cause access, following Intel's
[8255x manual](https://media.digikey.com/pdf/Data%20Sheets/Intel%20PDFs/8255x%2C82562%20Ethernet%20Controller%20Developer%20Manual.pdf)
sections 6.3.2 and 8.4. The latter preserves event masks and never acknowledges
causes before the miniport ISR. Both require exact-revision validation. Native
engine-stop hooks run before PCI decode removal; the independent command fence
always follows. `no_idle_proof` reports false explicitly, so uncertain backing
remains pinned instead of inferring idle from PCI readback or PM capability.
The flag emulator currently models virtual IF, without an architectural STI
one-instruction interrupt shadow. A profile requiring that precise pattern must
stay disabled pending the lead's supported-pattern review.
