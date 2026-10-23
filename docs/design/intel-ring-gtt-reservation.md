# Intel i830–i915G ring and GTT reservation proposal

Directive: [f2-107](../directives/f2-107-intel-i830-i915.md).
Prepared on 2026-10-23 after checking workstation `date`.
Status: **lead-reviewed; implementation and host evidence below**. The lead
decisions at the end supersede the original stolen-memory proposal. VBE
remains the mode provider. Host register fixtures do not qualify silicon.

The family census and command semantics are absent from `wt/f2-107` at its
part A base. Their relevant sections, including both sets of lead decisions,
were read from the read-only lead checkout at
`/home/peronslayer/Desktop/CiukiOS/docs/design/`. They are not copied here or
silently substituted with an older specification.

## Primary sources and repository comparison

The selected donor is Haiku **r1beta5**, commit
`a1f00118db8b858feb73f64ac4dc3d038b82bf9b`:

| Source | Evidence and port decision |
| --- | --- |
| [engine.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/engine.cpp), `QueueCommands`, `setup_ring_buffer`, idle and drawing hooks | Tail publication follows a locked write drain; packets are 8-byte aligned. The ring-space timeout logs and continues, and idle timeout has no failure return. Replace both behaviours; never submit after a failed wait or report that timeout as success. |
| [commands.h](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/commands.h), `xy_command` and derived packets | FILL/COPY use GTT-relative framebuffer offsets and packed XY/pitch fields. Translate inclusive donor rectangle endpoints. Do not import C++ constructors or their debugger failure path. Packed 24 bpp is not a donor format and initially punts before submission. |
| [intel_extreme.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/intel_extreme/intel_extreme.cpp), `intel_extreme_init` | Ring allocation is 16 pages through `gGART->allocate_memory`; cursor allocation uses a physically contiguous page. The donor explicitly changes PCI command bits and power/clock state. Those changes are outside this port's permission and will not be copied. |
| [intel_extreme.h](https://github.com/haiku/haiku/blob/r1beta5/headers/private/graphics/intel_extreme/intel_extreme.h), memory, ring, cursor and command definitions | Primary ring registers are at `0x2030` (TAIL, HEAD, START, CONTROL at offsets 0, 4, 8, 12). A matching PCI ID alone does not qualify stolen memory or cursor addressing. |
| [intel_gart.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/busses/agp_gart/intel_gart.cpp), `determine_gtt_stolen`, `determine_gtt_size`, `intel_map`, `intel_create_aperture` | The donor has a separate host-bridge/GART owner. It enables translation and writes scratch mappings outside the stolen portion. Ciuki has no equivalent allocator at this branch. Do not port this takeover to obtain a ring. |
| [cursor.cpp](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/intel_extreme/cursor.cpp) | The donor uses 64-pixel mono planes, a base register and palette entries. Its common code is not sufficient proof of cursor format/addressing for every target generation. Ciuki legacy mask semantics require explicit conversion. |
| [Intel 855GM/GME datasheet, 252615-005](https://www.intel.com/content/dam/doc/datasheet/855gm-gme-chipset-graphics-memory-controller-hub-datasheet.pdf), §§4.8.14, 4.11.6, 4.11.10–11, 5.4.2 | GGC selects BIOS-preallocated memory; its physical position is TOM minus TSEG minus UMA. The aperture is a different address. Function 1 can be non-VGA class `03:80`; gen2 GMADR is BAR0 and MMADR is BAR1. Reserved GMS values must be refused. This chipset datasheet is not a complete blitter retirement/stop specification. |
| [Linux v6.7 early-quirks.c](https://github.com/torvalds/linux/blob/v6.7/arch/x86/kernel/early-quirks.c), `i830_stolen_base`, `i845_stolen_base`, `i85x_stolen_base`, `i865_stolen_base`, `gen3_stolen_base` | Base decoding differs: TOM/TSEG calculation for 830/845/855, TOUD plus TSEG for 865, BSM for 915. Never apply one generation's register or formula to all five. |
| [Linux v6.7 intelfbhw.c](https://github.com/torvalds/linux/blob/v6.7/drivers/video/fbdev/intelfb/intelfbhw.c), `intelfbhw_get_memory`, `do_flush`, `intelfbhw_do_sync`, `reset_state` | Historical usable-stolen calculation subtracts GTT plus a 4 KiB popup reserve. Aperture length is not allocated backing. Flush then ring-empty is the donor's synchronization approximation; its own comment acknowledges limitations. Do not treat clearing ring enable as a proof that outstanding memory writes stopped. Do not copy its fence clearing. |
| [X.Org Intel 2.9.1 i830_memory.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_memory.c), `i830_get_gtt_physical`, `i830_get_stolen_physical`, `i830_allocate_ringbuffer` | Read and check actual PTE physical mappings and contiguity. Existing stolen mappings avoid allocating ordinary system RAM, but still require an allocator's positive ownership. This historical Git mirror is a reference, not the chosen engine donor. |
| [X.Org Intel 2.9.1 i830_accel.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_accel.c), `I830WaitLpRing`, `I830Sync` | The ring-space calculation retains an 8-byte gap. Synchronization delegates to `intel_batch_wait_last`, with a buffer-manager/kernel dependency; this is not a standalone idle proof. Its fatal timeout path is not imported. |
| [Linux v6.7 i915_gem_stolen.c](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gem/i915_gem_stolen.c), GTT exclusion and stolen resource setup | GTT backing can lie inside stolen memory and must be excluded by its actual address. BIOS/E820 reservation and suballocation ownership are separate checks. Newer-generation reserved-stolen registers are not gen2/gen3 evidence. |

Research source files, with their original notices, are only in ignored
`build/research/f2-107/`. No upstream source is imported into the kernel yet.
The Haiku files inspected retain their MIT notices and authors; an eventual
adapted file must retain the corresponding notice, author list and pinned
origin, with the MIT permission text and credits. Linux/X.Org references are
not silently made part of the import closure.

The owner's Windows 98 Resource Kit `wrkc24_3.htm`, “Display Driver Overview”,
and `wrkc26_5.htm`, “Setting Graphics Compatibility Options”, were read in
`/home/peronslayer/Desktop/CiukiOS/build/rskt`. They establish incremental
minidriver acceleration with DIB-engine fallback. They do not establish a
reservation protocol for this hardware. No proprietary content is copied.

## Device and framebuffer ownership

Only these primary vendor/device pairs nominate candidates:

| Primary PCI ID | Family | Framebuffer BAR | MMIO BAR | Stolen-base reference |
| --- | --- | --- | --- | --- |
| `8086:3577` | i830M integrated graphics | 0 | 1 | TOM minus TSEG minus stolen size |
| `8086:2562` | i845G | 0 | 1 | TOM minus TSEG minus stolen size |
| `8086:3582` | i855GM | 0 | 1 | TOM from host function 1, minus TSEG minus stolen size |
| `8086:2572` | i865G | 0 | 1 | TOUD plus TSEG |
| `8086:2582` | i915G | 2 | 0 | Graphics-function BSM |

`8086:2782` is the census's secondary i915 function, **not** another engine
candidate. No i915GM/945 or newer device is added through donor group masks.
A T23's 830MP host chipset never nominates this driver without an actual
listed graphics function. Host-bridge identity must also match the family.

The current `display_driver.framebuffer_bar` is fixed per descriptor. Use
separate declarative gen2/gen3 descriptors sharing the bounded driver ID and
display name; retain family-specific match tables. No PCI-ID policy moves to
generic code. Check class/subclass/function, enabled memory decode, BAR type,
alignment and extent without destructive BAR sizing or PCI configuration
writes. Validate the active fbdev resource handle/generation and the entire
visible extent, not only its first address. Read the active plane's base,
pitch, layout and enabled state and check its GTT mappings against that extent;
refuse tiled/fenced or ambiguous layouts rather than resetting their fences.

`registry.c` currently refuses unless exactly one class-3 PCI function exists.
Thus primary/secondary pairs intentionally remain on software under part A.
**Lead decision needed:** retain this conservative refusal initially, or
authorize a generic ownership-based selection change, with tests retaining
refusal for genuinely ambiguous owners. Merely deleting the display count
check or ignoring every non-VGA function is not an ownership proof.

## Reservation and GTT invariants proposed for approval

1. Keep physical stolen DRAM, GPU aperture offsets and CPU aperture addresses
   distinct in all records and arithmetic. Decode the family's exact GMS/base
   registers read-only; verify bounded, nonwrapping intervals below 4 GiB.
   Refuse disabled, local-only, reserved or undocumented configurations.
2. Reconcile the complete stolen interval with E820 and existing boot/kernel
   reservations **before** considering a ring. Any overlap with allocatable
   RAM, loaded kernel/modules, page tables, allocator pages, SMRAM/TSEG or a
   conflicting reservation refuses acceleration. Never retroactively remove
   potentially allocated PMM pages. E820-reserved does not mean driver-owned.
3. Read translation enable/base, the GTT alias and relevant PTEs without
   changing them. Check actual table-backing and popup/firmware reserved
   intervals, using the family's documented layout rather than only a size
   subtraction. Preserve every existing PTE, page-table control register and
   fence. No GART takeover, scratch replacement, new translation mapping or
   normal-RAM DMA allocation is allowed in this first transport.
4. Proposed ring size: **64 KiB**, page aligned, matching the H donor. Proposed
   private completion/cursor storage: one **4 KiB** page each, reserved
   separately only if their independently reviewed implementation uses them.
   Every byte must come from positively owned usable stolen memory. A zero
   pixel pattern, invalid PTE, free-looking aperture tail, firmware-reported
   VRAM length or successful `registry_claim` alone is insufficient proof.
5. Required ownership input is a kernel-private, generation-bound reservation
   ledger identifying driver-allocatable stolen spans and all firmware-held
   exclusions. Its producer must have established ownership before exposing
   any span. This branch's boot display record has only VBE metadata and
   `vram_bytes`; it provides no such handoff. **Lead decision needed:** approve
   a reservation producer/handoff and its scope, or explicitly keep activation
   refused until one exists. This proposal does not invent a new boot ABI or
   assume undocumented BIOS allocations are free.
6. Given that positive ledger, choose the highest page-aligned 64 KiB GPU
   interval wholly inside its allocatable spans whose existing valid PTEs map
   exclusively to usable stolen pages. Verify every PTE, mapped address,
   mapping type and alias, not merely the first/last. Exclude the visible
   frame, all active planes/cursors/overlay/compression buffers, pre-existing
   rings/status pages, table backing, popup and all other ledger reservations
   in both GPU-offset and physical-address spaces. Refuse unknown state.
7. Claim both the aperture range and physical stolen backing, plus validated
   MMIO ranges, under pinned driver ownership. A registry claim expresses a
   suballocation only after invariant 5 proves it free; it is not that proof.
   Failure before hardware activation unwinds claims/mappings. Check all
   generations again under the presenter before touching owned hardware.
8. No GTT entry becomes driver-writable: entries for the visible frame and
   reserved ring/private pages are pinned as read-only translation evidence.
   All other entries remain firmware-owned and unchanged. Recheck pinned PTEs
   and relevant firmware state at activation and mode boundaries. No public
   offscreen rows are exposed initially. Private pages are never user-mapped,
   presented as a surface, or included in a software fallback target.
9. Refuse active or unproved firmware ring work. Starting a foreign ring to
   drain it, disabling it to discard work, or adopting its memory is not probe.
   Probe performs no engine/MMIO/configuration write, claim, pixel test or
   marker mutation. It only nominates. Mapping/claims belong to `start` after
   generic safe-mode/`accel=0` gates and the durable attempt marker.
10. Preserve firmware-programmed GGC, stolen base/size, GTT base/PTEs, fences,
    HWS, secondary rings, interrupt routing/masks, PCI command/power, clocks,
    PLL/panel, scanout and VBE mode. The only eventual register changes are
    the explicitly acquired disabled primary ring and separately qualified
    cursor plane. Never enable PCI bus mastering or an AGP device. If a
    required firmware setting is absent, refuse rather than enable it.

## Execution, retirement and quarantine constraints after approval

Activation is separate from command construction. It first verifies a
disabled, documented-idle engine under `fbdev_accel_probe`, saves the original
disabled ring state, publishes an unsafe CPU guard before any potential
writer, and configures only the owned ring. A bring-up refusal before writes
is distinct from an unsafe failure after activation.

Build an entire validated packet plus alignment/flush/completion overhead
before publishing it. Reserve an 8-byte empty/full gap, account for wrap and
NOP padding, and never overwrite unread bytes. Validate head/tail alignment
and bounds. Drain CPU writes before updating TAIL; read back posted MMIO as
required. No arbitrary user packets, pointers, relocations, addresses or
host-memory operands enter the ring.

All waits need an absolute monotonic deadline and a frozen-clock iteration
cap. Completion observes the presenter's 1 ms slices without losing the
drawing claim or accepting stale display generations. Success requires an
audited generation-specific proof that destination writes are visible to the
CPU; HEAD == TAIL alone and the donor's timeout-and-log loop are insufficient.
**Lead review item:** the inspected H and LI sources do not close that proof
for every generation. Confirm documented retirement/idle evidence (or an
ordered completion write within an owned stolen page) before enabling any
generation. No speculative completion opcode or idle bit is approved here.

On timeout or invalid state, disable submissions and quarantine for this boot.
`cpu_ready` is cached, false after unproved activity, and never waits. Keep
all resources and the rejecting guard pinned until a documented bounded idle
or stop proof succeeds. Clearing ring enable, clearing PCI bus mastering, or
reaching the timeout is not that proof. Do not reset foreign state or introduce
`panic()`. Serial diagnostics remain available. If no stop proof is available,
the driver remains unsafe and software drawing cannot resume. This is a
refusal condition for activation qualification, not a promise that every
wedged engine can be recovered.

After proven idle, restore only acquired state, revoke private/offscreen
leases, and permit CPU fallback. `-EOPNOTSUPP` guarantees no submission/pixel
change. A failed submitted overlapping COPY returns the reconstruction error
and is never replayed blindly. VBE/fullscreen entry first stops this engine;
every return to VBE invalidates the reservation ledger and requires a fresh
ownership check. No same-boot reprobe follows quarantine.

## Command and evidence plan after review

- FILL, COPY and SYNC are mandatory. Initially accept only validated linear
  16/32 bpp modes with representable pitches and coordinates. COPY needs
  source-snapshot semantics for all overlap directions; use a documented
  overlap-safe command or bounded ordered strips, never infer safety from
  the donor's single positive-pitch packet.
- The donor's INVERT waits for a real optional hook/capability in the agreed
  part C integration. Part A has no INVERT hook: do not change its public ABI
  or misuse PATTERN as an INVERT command. Other unsupported operations punt.
- Qualify mono cursor addressing/control independently per generation. Stage
  and validate before hiding; translate legacy opaque-mask/image into the
  donor's AND/XOR planes, initialize unused 64x64 pixels as transparent, test
  palette and signed positions, and leave SET hidden. Never advertise ARGB
  from this donor's mono cursor. Unproved cursor disable retains the guard.
- Fake-register tests: all PCI/refusal and reservation paths above; zero
  writes on probe; exact ring setup/tail ordering; capacity/full-gap/wrap;
  normal and frozen-clock timeouts; fill; both COPY overlap directions on
  both axes; sync visibility; partial submission/quarantine/stop failures;
  claims/generation rollback; mode transitions; cursor plane conversion,
  hide/load/move/show/stop and refusal. Use swdib pixel goldens and canaries
  without modelling a packet decoder from the implementation under test.
- DM9 uses the generic native-only comparison with fallback recorded
  separately, exclusive drawing ownership, saved/restored pixels only after
  idle, deadline/cancellation and independent cursor register evidence.
  Register fixtures cannot qualify real silicon or prove bus recovery.
- Register source files explicitly in `scripts/build_kernel.py`, use unique
  objects, and keep driver display names in the registry descriptor so Device
  Manager/Control Panel consume the generic record. Preserve DM12 checks.
  The family result remains **pending transport review**, not accelerated.

No ABI/SDK header change or production `.c` inclusion is proposed at this
stage. If later changes require one, f2-107's ABI oracle/regeneration and
duplicate-symbol integration rules apply before handoff.

## Lead decisions (2026-10-23 00:51)

1. **Reservation authority: no stolen-memory suballocation.**
   - **Placement.** The ring buffer, hardware status page and batch scratch live in pages the kernel allocates from its own physical allocator, pinned in the DMA ledger with an owner record. They are mapped through GTT entries at the top of the aperture that are proved unused: each entry is invalid or points to the firmware scratch page, and the range lies above the active frame buffer and above every entry the firmware made valid.
   - **Lifetime.** The original entries are saved and restored on stop and quarantine.
   - **Refusals.** Stolen memory is never written except through the active frame buffer. When no such free GTT range can be proved, activation is refused and the software engine draws. This is the same positive-ownership approach as the Linux intel-gtt driver, applied without a GART allocator.
2. **Secondary functions:** the conservative refusal stays. `2782` and other secondary class-3 functions are never independent engines.
3. **Completion and stop.**
   - **Completion.** Each submission ends with a store of a sequence number to the hardware status page (MI_STORE_DATA_INDEX or the generation's equivalent, cited per generation), waited for with the bounded, sliced presenter waits.
   - **Stop.** A flush, then ring HEAD equal to TAIL, then the generation's idle bits, all within a deadline. Any failure quarantines the engine and keeps the ring pages pinned until reboot; the software engine draws only after the CPU-safety proof of the display contract's Amendment 1.
   - **Missing evidence.** A generation without a cited completion and stop sequence stays refused.
4. **INVERT** waits for f2-101d (part C). The cursor stays gated per generation.

Implement the 2D blitter commands now on these terms, with host tests that model the GTT, the ring, the status-page breadcrumbs, a stop timeout and restoration.

## Implementation evidence and conservative qualifications

The implementation uses the reviewed kernel-owned-page alternative, not
items 3–8 of the original stolen-memory proposal. It only borrows **invalid**
GTT entries: this branch has no authoritative firmware scratch-page identity.
The chosen interval is at the aperture top, above the entire visible extent
and every valid firmware PTE. All original entries are saved. No valid PTE,
stolen byte outside the visible framebuffer, translation control, PCI command,
fence, interrupt mask or scanout register is written.

Additional primary-source checks before implementation:

- [Linux v6.7 gen2_engine_cs.c](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/gen2_engine_cs.c),
  `__gen2_emit_breadcrumb` / `gen3_emit_breadcrumb`: MI_FLUSH followed by 16
  scratch and 8 sequence MI_STORE_DWORD_INDEX writes. Use those counts, replace
  the final interrupt with NOOP (polling only), and additionally require empty
  ring plus parser idle. [intel_engine.h](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_engine.h)
  reserves driver HWS dwords 0x30 onward; sequence uses 0x40, scratch 0x80.
  It also documents asynchronous ISR-copy writes to HWS. Require firmware's
  existing HWSTAM low 16 bits all masked, without changing that mask, so an
  asynchronous interrupt cannot write the private HWS after stop/free.
- [intel_ring_submission.c](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_ring_submission.c),
  `set_hws_pga`: gen2/3 HWS uses a **physical** address, even though its page
  also has an owned GTT entry. Never pass the aperture offset to HWS_PGA.
- [intel_engine_cs.c](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_engine_cs.c),
  `ring_is_idle`, and [intel_engine_regs.h](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_engine_regs.h):
  gen3 requires HEAD == TAIL and MI_MODE.MODE_IDLE (bit 9). The source explicitly
  has no gen2 idle bit. **3577/2562/3582/2572 remain refused**, with an explicit
  reason, until separate gen2 stop evidence exists; no gen3 bit is inferred.
- [intel-gtt.c](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-gtt.c),
  `i830_write_entry`, `intel_i915_setup_chipset_flush`, `i9xx_chipset_flush`:
  ordinary PTEs use physical page | 1. CPU cache cleaning and chipset write
  buffer flushing are separate. Require CLFLUSH and a firmware-enabled host
  IFPADDR page, claim/map that page, clean WB ring/HWS storage, drain WC writes,
  and write the flush page before tail publication. Never enable IFPADDR.
- Intel **301467-005**, §§9.1.11–13, 9.1.26–27 (pp. 181–188),
  [Intel-authored datasheet transcription](https://dtsheet.com/doc/657160/intel-915gl):
  fixed MMADR/GTTADR extents are 512/256 KiB; MSAC at PCI 0x62 selects 128 MiB
  when bit 1 is set, otherwise 256 MiB. Use that read-only size, not VBE VRAM
  or destructive BAR sizing. BSM and GGC qualify stolen framebuffer backing.

COPY uses an owned 32 KiB scratch scanline and two ordinary positive-pitch XY
source blits per row, separated by MI_FLUSH. Process rows bottom-up for a
downward overlap, otherwise top-down. Full source-row staging preserves
horizontal snapshot semantics without relying on undocumented reverse BLTs.
FILL, each row pair and SYNC end with the breadcrumb protocol. An entire call
shares one deadline and a frozen-clock poll cap; waits yield through the
presenter pause hook at most every 1 ms and revalidate leases after resumption.
The ring retains the donor's 8-byte gap and conservatively leaves its final
64 bytes as NOOPs, as described in Linux intelfbhw.h.

Stop submits a flush/breadcrumb when healthy, then proves completion, empty
ring and gen3 parser idle before disabling the owned ring and restoring its
registers, HWS and GTT entries. A failed proof retains all mappings, claims
and DMA pins; cpu_ready remains false. Quarantine does not equate to idle.
After quarantine, stop only observes the existing submission; it never queues
another command. Restored state is read back before DMA pages may be freed.
Mode stop releases all private state; any later start rechecks ownership and
allocates a fresh reservation. Quarantine forbids same-boot restart.

INVERT waits for part C. Cursor, pattern, upload and offscreen capabilities
are absent; unqualified cursor state is never advertised or modified.

## Amendment 1: Gen2 research and per-chip protocol

Researched on 2026-10-23 after running `date -Is`, before changing code.
This section supersedes the blanket Gen2 idle refusal above. Sources are
downloaded only into `build/research/f2-107/amendment1/` (earlier pinned Linux
files remain in its parent). Downloads use the GitHub connector because shell
DNS is unavailable. No unavailable programmer's reference is claimed as read.

| Primary source, pinned tag and original file lines | Finding and implementation decision |
| --- | --- |
| X.Org **xf86-video-intel-2.1.1**, [`src/i830_accel.c:94–163,168–221`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/xf86-video-intel-2.1.1/src/i830_accel.c#L94) | `I830WaitLpRing` retains eight bytes; `I830Sync` sends MI_FLUSH with dirty-state/map-cache flags, then waits for an empty ring. Its progress-reset timeout and FatalError are excluded. The later 2.9.1 donor delegates completion to the kernel. |
| Same X.Org tag, [`src/i830_debug.c:732–759`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/xf86-video-intel-2.1.1/src/i830_debug.c#L732) | INSTDONE is read for error diagnosis, not used by I830Sync as a completion fence. Do not turn diagnostic output or HEAD alone into a CPU-visibility proof. |
| Linux **v6.7**, [`gt/gen2_engine_cs.c:16–41,148–179,293–309`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/gen2_engine_cs.c#L16), [`gt/intel_ring_submission.c:1074–1098,1147–1185`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_ring_submission.c#L1147) | Gen2 selects gen3_emit_breadcrumb: MI_FLUSH, 16 scratch stores and eight sequence stores. Gen2 flush also emits 12 scratch-store / MI_FLUSH-with-write-inhibit pairs between two full flushes. Adapt both sequences; use polling instead of the final user interrupt. Use that flush between staging and reading a COPY scratch row too. |
| Linux **v6.7**, [`i915_irq.c:990–1037`, `i8xx_irq_handler`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/i915_irq.c#L990), and ring-submission lines above | i8xx uses 16-bit IIR/IMR and signals the engine's breadcrumb processing on USER_INTERRUPT. An interrupt is notification, not the fence. Ciuki changes no IRQ mask and emits no interrupt; it polls its own physical HWS sequence. Existing HWSTAM masking remains mandatory. |
| Linux **v6.7**, [`gt/intel_engine_cs.c:1851–1874`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_engine_cs.c#L1851), [`gt/intel_ring_submission.c:51–73`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_ring_submission.c#L51) | No Gen2 MODE_IDLE bit exists in this idle helper. Preserve physical HWS addressing. Require the breadcrumb, HEAD == TAIL, and the Gen2 INSTDONE execution/cache bits instead of borrowing Gen3 bit 9. |
| Intel GPU Tools **intel-gpu-tools-1.5**, [`lib/instdone.c:329–356`](https://github.com/freedesktop-unofficial-mirror/xorg__app__intel-gpu-tools/blob/intel-gpu-tools-1.5/lib/instdone.c#L329), [`lib/intel_reg.h:403–429`](https://github.com/freedesktop-unofficial-mirror/xorg__app__intel-gpu-tools/blob/intel-gpu-tools-1.5/lib/intel_reg.h#L403), [`lib/intel_chipset.h:193–196`](https://github.com/freedesktop-unofficial-mirror/xorg__app__intel-gpu-tools/blob/intel-gpu-tools-1.5/lib/intel_chipset.h#L193) | Explicit Gen2 membership and INSTDONE decoding: execution/cache units occupy bits 6–24 (including blitter bit 7 and map L2 bit 6). Require mask `0x01ffffc0` on enabled Gen2 chips. Exclude GMBUS/FBC and ring-selection status; these are not independent evidence of completion of our BLTs. This mask plus the ordered HWS store is Ciuki's conservative inference from the cited definitions and Linux retirement sequence, not a claimed Intel-specified composite mask. |
| Linux **v6.7**, [`drivers/char/agp/intel-gtt.c:698–731,789–802,1191–1201`](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-gtt.c#L698), [`intel-agp.h:60–61,130`](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-agp.h#L60) | Gen2 has no i915 IFPADDR interface. WBINVD precedes HIC (MMIO 0x70) bit 31 and a wait for self-clear. The source identifies documentation for 855GM and observed success on 845G, explicitly only hoping for the other chips. Enable 845G/855GM; keep 830M/865G refused with `gen2_chipset_flush_unqualified`. Bound HIC waits and propagate timeout; Linux's timeout-then-continue is excluded. This uniprocessor kernel uses its existing WBINVD helper. GTT alias is MMADR + 0x10000; claim register and table subranges separately. |
| Linux **v6.7**, [`arch/x86/kernel/early-quirks.c:258–324,352–374`](https://github.com/torvalds/linux/blob/v6.7/arch/x86/kernel/early-quirks.c#L258), [`include/drm/i915_drm.h:83–95`](https://github.com/torvalds/linux/blob/v6.7/include/drm/i915_drm.h#L83), [`intel-gtt.c:524–546`](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-gtt.c#L524) | Decode 845 TOM/TSEG and 855 function-1 TOM/TSEG, not BSM. 845 uses i830 GMS encodings; 855 uses 1/4/8/16/32 MiB. GGC bit 0 selects 64/128 MiB aperture. Revalidate every source register and function identity throughout the lease. No stolen allocation or memory-limit change. |
| Intel **252615-005**, [855GM/GME datasheet](https://www.intel.com/content/dam/doc/datasheet/855gm-gme-chipset-graphics-memory-controller-hub-datasheet.pdf), §§4.8.14,4.11.10–11,5.4.2 | Confirms BAR0 aperture, BAR1 512 KiB MMIO decode and reserved preallocation. Claim disjoint register offsets 0–64 KiB, the GTT alias, and plane registers 0x70000–0x71fff within that BAR. This public chipset datasheet does not specify the execution/HIC protocol; the pinned upstream implementations above supply that evidence. A separate 830/845/855/865 graphics PRM was not retrievable. |

The ring/HWS/scratch reservation remains 25 ledger-owned pinned pages in the
highest invalid GTT entries. Gen2 changes neither firmware translation enable
nor valid mappings. Healthy stop emits the generation flush/breadcrumb, waits
for the matching sequence, empty ring, Gen2 INSTDONE and HIC self-clear, then
disables and restores the acquired ring/HWS/GTT. Quarantined stop only observes
existing work. A failed flush, idle, ownership or restore proof retains the
guard and pages. No timeout enables CPU drawing or starts another command.

| Chip | Protocol gate after this amendment | Remaining qualification |
| --- | --- | --- |
| i830M / 3577 | Refused: `gen2_chipset_flush_unqualified` | Chip-specific HIC/alternative chipset-flush evidence |
| i845G / 2562 | Candidate: Gen2 HWS + INSTDONE + bounded HIC | All ownership/firmware checks and hardware DM9 |
| i855GM / 3582 | Candidate: Gen2 HWS + INSTDONE + bounded HIC | All ownership/firmware checks and hardware DM9 |
| i865G / 2572 | Refused: `gen2_chipset_flush_unqualified` | Chip-specific HIC/alternative chipset-flush evidence |
| i915G / 2582 | Existing Gen3 candidate | Hardware DM9; existing IFPADDR prerequisite |

These are activation candidates, not hardware qualification results. Cursor
and optional commands remain gated; public ABI/SDK headers are unchanged.

## Amendment 2: i830M and i865G evidence audit

Researched on 2026-10-23 after workstation `date -Is`, before changing code.
Amendment 1's i845G/i855GM protocol is approved by the lead. This audit leaves
those candidates and i915G intact. It separates GPU cache retirement from
CPU-to-GPU chipset publication: an MI_FLUSH/HWS breadcrumb proves neither
the existence nor the self-clear semantics of a chipset's HIC bit.

| Primary source and pinned location | Finding / decision |
| --- | --- |
| Intel **252514-005**, [82865G/82865GV datasheet](https://www.mouser.com/catalog/specsheets/intel%20corporation_25251405.pdf), §§3.5.25,3.7.10–11,5.4.2 (printed pp.77,106–107,172) | BAR0 is the graphics aperture; BAR1 allocates 512 KiB for graphics registers. The BLT overview establishes functionality, not retirement or HIC semantics. AGPCTRL bit 7 documents a write-buffer drain when changing external-AGP translation state; it is not evidence for IGD HIC at MMIO 0x70. Do not change firmware AGP/GTT enable state or introduce PCI configuration writes to obtain a different flush. HIC and MI_FLUSH are absent from the PDF text search; this is a search result, not proof that the hardware lacks them. |
| Intel **298338-003**, [82830 chipset-family datasheet, distributor copy](https://e-nexty.dxp.nexty-ele.com/en/product_files/download?lc_code=ja&maker_code=INT&product_file_id=705503&product_id=2425659&product_part_number=82830MP&search_log_id=6197057), §4.5.3.11 (indexed excerpt) | The indexed primary-document excerpt confirms BAR1's 512 KiB MMADR allocation. Full-PDF retrieval failed from Intel and the distributor; the Datasheet Archive redirect also failed. No full-text HIC/completion audit or unavailable graphics PRM is claimed. The available excerpt supplies no HIC evidence. |
| X.Org **xf86-video-intel-2.1.1**, [`src/i830_accel.c:168–221`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/xf86-video-intel-2.1.1/src/i830_accel.c#L168) | I830Sync/I830EmitFlush use MI_FLUSH with dirty-state/map-cache flags for all four Gen2 chips; only i965 changes these flags. There is no i830M-versus-i865G chipset-flush proof here. Waiting for ring space is weaker than Ciuki's existing breadcrumb/INSTDONE proof. |
| X.Org **2.9.1**, [`src/i830_batchbuffer.c:42–70,104–150,173–200`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.9.1/src/i830_batchbuffer.c#L104) | i830/i845 use MI_BATCH_BUFFER with an end address in the non-DRM path; i865 uses MI_BATCH_BUFFER_START and restricts batch BOs to one page. Those are batch-dispatch differences, not different chipset-flush interfaces. Ciuki emits only inline ring packets, with no batch command or batch BO. A 64 KiB ring is not a 64 KiB batch. |
| X.Org **2.21.15**, [`src/uxa/intel_batchbuffer.c:70–78,179–219`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-intel/blob/2.21.15/src/uxa/intel_batchbuffer.c#L179) | MI_FLUSH_DW is selected only for a BLT batch on Gen6 or newer. Gen2 uses MI_FLUSH. Do not substitute MI_FLUSH_DW to enable i830M/i865G. The i865 page-sized batch restriction persists independently. |
| Linux **v6.7**, [`drivers/gpu/drm/i915/i915_pci.c:85–135`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/i915_pci.c#L85), [`gt/intel_ring_submission.c:1147–1185`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/intel_ring_submission.c#L1147), [`gt/gen2_engine_cs.c:16–41,144–179,183–249`](https://github.com/torvalds/linux/blob/v6.7/drivers/gpu/drm/i915/gt/gen2_engine_cs.c#L16) | Both chips are Gen2, require physical HWS and lack coherent GGTT. They select the existing Gen2 MI_FLUSH sequence and 16+8 HWS breadcrumb. i830/i845 additionally select the batch TLB workaround; i865 does not. No batch dispatch is imported. These sources support GPU retirement, but do not independently qualify chipset publication. |
| Linux **v6.7**, [`drivers/char/agp/intel-gtt.c:698–731,1191–1201,1279–1288`](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-gtt.c#L698) | Both chips are routed through i8xx_gtt_driver and its WBINVD/HIC flush. The executable path supersedes the stale 1 KiB-buffer comment above it. The HIC comment identifies documentation only for 855GM and success only on 845G; other Gen2 chips remain an explicit assumption. A historical buffer-eviction heuristic, generic routing or a self-clearing fake register cannot qualify HIC on either remaining chip. |

**Decision:** neither remaining chip has a complete cited publication protocol
within the firmware-preserving reservation contract. Keep both refused before
marker I/O, MMIO/GTT access or allocation. Use chip-specific summary/Device
Manager reasons so this audit is distinguishable from a generic Gen2 refusal:

| Chip | Current gate / reason | Evidence still required |
| --- | --- | --- |
| i830M / 3577 | Refused: `i830m_chipset_flush_unproven` | Chip-specific HIC semantics or a proved firmware-preserving alternative; then its TOM/TSEG decoding and hardware DM9 |
| i865G / 2572 | Refused: `i865g_chipset_flush_unproven` | Chip-specific HIC semantics or a proved firmware-preserving alternative; then TOUD/TSEG decoding and hardware DM9 |
| i845G / 2562, i855GM / 3582 | Approved protocol candidates, unchanged | Firmware/ownership gates and hardware DM9 |
| i915G / 2582 | Existing Gen3 candidate, unchanged | Firmware/ownership gates and hardware DM9 |

The inactive chips' stolen-memory decode is deliberately not generalized from
845/855: the current adapter branches implement only the enabled candidates.
Opening either gate later requires the correct memory decoding too. No new
timeout, reset, stop, panic, IRQ, ABI or capability path is introduced here.

Host evidence adds complete refusal-lifecycle checks for each gated chip and
both aperture selections, with its own matching host bridge, including forced
marker state, hostile HIC/ring contents, summary reason and byte-for-byte
MMIO/GTT/DMA preservation. Transport refusal is checked independently before
callbacks or state changes. Existing enabled-chip packets, pixel goldens,
timeouts, stop and pin-retention tests continue to run.

New source downloads are confined to `build/research/f2-107/amendment2/`.
The GitHub connector supplied pinned text files; shell curl failed DNS (exit
6). Intel PDF evidence was inspected online, not downloaded elsewhere.
