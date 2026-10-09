# Device and firmware ownership under Ciuki VMM

Author: Codex. Reviewer and integrator: Claude (lead). Status: approved
after the Claude–Codex cross-review, 2026-10-09.

## Decision

Ciuki VMM owns hardware access after loader handoff. Drivers, firmware and
DOS VMs receive explicit, revocable services; none independently owns the
physical PIC, timer or DMA allocator. This implements D5, D9 and D11 of the
[binding decision](../../dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md).
F0 establishes ownership and observable failure paths, without requiring DOS
emulation or runtime BIOS calls. Phase tags follow decision D10: F0
foundations, F1 storage/input/safe mode, F3 DOS device mediation, F4 audio
and optional graphics acceleration, and F4+ for power management, which
needs its own later decision. (Phase allocation integrated by the lead on
2026-10-09; the first draft used local tags F2/F3/F4 for these three areas.)

## Sources

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  system programming: interrupt handling, V86 protection and SMM.
- [PCI Local Bus Specification 2.2](https://www.ics.uci.edu/~harris/ics216/pci/PCI_22.pdf),
  sections 2.2.6, 6.2.4–6.2.5; Linux v6.12 implementations linked below.
- [Intel 8259A datasheet, p. 18](https://www.alldatasheet.net/html-pdf/66107/INTEL/8259A/2317/18/8259A.html):
  level requests, default IRQ7 and in-service detection.
- [VESA VBE 3.0](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf):
  mode information, linear framebuffer and firmware entry interfaces.
- [ACPI software model](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html)
  and [Intel/Microsoft APM 1.2](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF):
  firmware/OS handover and power-event responsibilities.
- [IBM T20–T23 maintenance manual](https://thinkpads.com/support/hmm/hmm_pdf/62p9631.pdf),
  power management, printed pp. 28–31; [Compaq E500/V300 service guide](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf),
  sections 1.2 and 2.5. These describe machine families, not measured routing
  or firmware behavior of the owner's units.

Research and repository comparison: 2026-10-09. Requirements below are design
decisions, not claims of completed qualification. Upstream references establish
behavior; code reuse still requires the license audit in
`foundations-transition.md`.

## Resource registry and lifecycle

**[F0]** One kernel registry MUST record resource type, owner/generation,
origin, interval or channel, sharing policy and lifecycle state. PCI records
MUST include bus/device/function, vendor/device/subsystem/revision, class,
header type, command register, BAR type/base/known size, bridge windows and
Interrupt Line/Pin. Unknown size or routing MUST remain explicitly unknown.
Enumeration MUST NOT activate a driver.

**[F0]** PCI configuration mechanism #1 MUST serialize the `0xCF8` address
and `0xCFC–0xCFF` data transaction with interrupts saved/restored. Use
`0x80000000 | bus<<16 | device<<11 | function<<8 | (offset&0xFC)` for the
first 256 bytes only. Scan function zero, honor multifunction headers and
follow firmware-configured bridge buses with cycle/range checks. Reject vendor
`0xFFFF`; do not assign bus numbers or relocate resources. This follows
[Linux `pci_conf1_read/write`](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/pci/direct.c).

**[F1]** Before activation, resource sizes MUST be established from validated
firmware information, documented device apertures or controlled BAR sizing.
Sizing MUST save configuration, quiesce the function, disable its I/O/memory
decoding and bus mastering, probe, then restore and verify. It MUST NOT touch
an active console, firmware-owned USB controller or unknown bridge. Decode
64-bit BAR pairs with overflow checks; reject inaccessible addresses above
4 GiB. Do not overwrite adjacent write-one-to-clear Status bits when changing
Command. [Linux `__pci_read_base`](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/pci/probe.c)
demonstrates why reading a BAR alone cannot establish its size.

**[F0]** The registry MUST reserve legacy resources before PCI claims:

| Owner | Ports/resources |
| --- | --- |
| PIC service | `20–21`, `A0–A1`; IRQ2 cascade; ELCR `4D0–4D1` only on supported chipsets |
| Timer service | `40–43`; IRQ0; channel 2 and shared port `61` arbitration |
| RTC/platform service | `70–71`; IRQ8; CMOS index/NMI-mask shadow; port `92` A20/reset policy |
| Input service or firmware lease | `60`, `64`; IRQ1/IRQ12 |
| DMA service | `00–1F`, `80–8F`, `C0–DF`; channels 0–7, channel 4 reserved for cascade |
| PCI service | `CF8–CFF` |
| Conditional devices | Validated VGA/ROM/framebuffer extents, UART ports, ATA command/control ranges and IRQs |

All port numbers above are hexadecimal. **[F0]** The loader MAY probe fw_cfg
`510–511` only after validated SMBIOS identifies a QEMU platform and the range
has no conflict; reserve it and verify the `QEMU` signature before directory
reads. Physical notebooks MUST skip this probe. No fw_cfg DMA or diagnostic
exit port is required. [QEMU fw_cfg specification](https://www.qemu.org/docs/master/specs/fw_cfg.html).
**[F0]** Serial output MUST claim a validated firmware-advertised UART range,
use a reversible presence probe and bounded polling, initially 38400/8N1 per
`f0-acceptance.md`. Missing hardware MUST fall back to screen output. Panic
polling MUST avoid locks and terminate on timeout.

Reservations are exclusion policy, not assertions that every port implements
a device. **[F0]** MMIO MUST be supervisor-only, excluded from RAM allocation
and mapped uncached initially; framebuffer cache optimization requires separate
qualification. Fixed aliases and shared register blocks MUST have one parent
owner, not conflicting child claims.

**[F0]** Claims MUST be transactional:
`discovered/firmware-reserved → claimed → active → quiescing → released`.
Failure enters `quarantined`, retaining resources whose safety is uncertain.
Exclusive overlap MUST fail without writes; a conflict report MUST identify
both owners, resource, range and reason. **[F1]** Release MUST reject new work,
stop device interrupts/DMA, drain callbacks, prove idle, revoke mappings and
only then free buffers/claims. Generations MUST reject stale completions.
Unknown devices MUST receive no VMM enablement. Firmware dependencies remain
reserved; blindly clearing every PCI bus-master bit is not safe isolation.

## Physical and virtual interrupts

**[F0]** Only the PIC service MUST program the 8259A pair: master vectors
`20–27`, slave `28–2F`, fixed priorities, explicit EOI, no auto-EOI. CPU
exceptions remain `00–1F`; syscall vector `80` belongs to `execution-abi.md`.
Local APIC/IOAPIC and MSI MUST remain unused through F4. All lines initially
remain masked; unmask only after handler/context publication. IRQ2 MUST be
unmasked whenever an enabled slave line needs it. Mask changes MUST preserve
other owners' bits under one short critical section. See
[Linux PIC initialization and masking](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/i8259.c).

**[F0]** The dispatcher MUST own physical acknowledgement and EOI; drivers
acknowledge only their device source. After service, send a specific EOI for
the accepted master IRQ, or slave IRQ followed by master IRQ2. For IRQ7,
read master ISR: clear bit 7 means spurious, no handler and no EOI. For IRQ15,
clear slave ISR bit 7 means spurious: no slave EOI, but acknowledge the
master cascade. Real IRQ7/15 receive normal service. ISR-selection accesses
MUST be serialized. These rules follow the
[8259A default-interrupt behavior](https://www.alldatasheet.net/html-pdf/66107/INTEL/8259A/2317/18/8259A.html).

**[F1]** Shared PCI INTx MUST use a bounded handler chain. Every participant
MUST declare sharing, test its own status and return `handled` or `not-mine`;
the dispatcher MUST visit all participants. Mask the line during service,
clear asserted device causes before EOI, then unmask after the chain completes.
If draining exceeds the interrupt budget, keep the line masked, issue EOI to
release the cascade, and defer source clearing before unmasking.
Persistent/unclaimed assertions MUST produce counters and quarantine; if the
culprit cannot be isolated, disable the entire chain and report affected users.
Removing one participant MUST synchronize pending callbacks and retain peers.
This adopts the separation in
[Linux generic IRQ handling](https://www.kernel.org/doc/html/v6.12/core-api/genericirq.html).

**[F1]** Interrupt Pin identifies INTA–INTD, not an ISA IRQ number; Interrupt
Line is firmware routing information, not proof of wiring. Activation MUST
validate bridge swizzling, routing and ELCR against the detected chipset or
firmware routing table. ISA edges MUST NOT share with PCI levels; fixed edge
lines MUST NOT be converted. Unknown routing means polling where documented,
otherwise disabled. [PCI 2.2, section 6.2.4](https://www.ics.uci.edu/~harris/ics216/pci/PCI_22.pdf).

**[F3]** Each DOS VM MUST own virtual PIC IRR/ISR/IMR, vector bases, cascade,
initialization and EOI state. Guest CLI, masks and EOIs MUST affect only that
state; physical completion MUST NOT wait for guest EOI. The virtual PIT MUST
advance while virtual IF is clear. Existing
`src/vm/guest_peripherals.c:cvgp_irq_acknowledge` and
`src/vm/session_scheduler.inc:vm_scheduler_irq` are review inputs: the latter
already services host time before Jemm tests guest IRQ0 eligibility. Its Jemm
callback is retired. The former's empty-slave branch returns master-base+7;
reuse MUST correct/test it against slave-spurious IRQ15 semantics. V86 trapping
is supported by [Intel SDM, V86 I/O protection](https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf).

## DMA and asynchronous ownership

**[F0]** DMA ports/channels MUST be inaccessible to user processes. Unused
8237 channels MUST remain masked after handoff. **[F3]** DOS DMA MUST be
emulated against validated private guest pages, never guest-supplied physical
addresses.

**[F1, before physical DMA]** The 8237 service MUST own channel allocation,
page registers and shared byte-pointer flip-flops. Programming MUST occur
masked under the controller lock. Buffers MUST be pinned, physically contiguous
and wholly below 16 MiB. Channels 0–3 MUST NOT cross 64 KiB boundaries;
channels 5–7 require even addresses/lengths and MUST NOT cross 128 KiB
boundaries. Counts encode transfers minus one; channel 4 is unavailable.
Invalid requests MUST split or use bounded bounce buffers, never truncate.
[Linux x86 DMA definitions](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/dma.h).

**[F1, before PCI DMA]** Each driver MUST declare its actual address mask,
alignment, boundary and descriptor constraints. PCI does not imply ISA's
16 MiB limit or unrestricted 32-bit addressing. Each DMA segment MUST be
physically contiguous; scatter/gather is allowed only when the device supports
it. Descriptors and payloads MUST remain pinned until completion or proven
shutdown. CPU/device ownership transfers need ordering barriers even for
coherent memory. [Linux DMA guide](https://www.kernel.org/doc/html/latest/core-api/dma-api-howto.html).
For example, [Linux `snd_es1968_create`](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/es1968.c)
requests a 28-bit mask: the E500 ESS backend MUST verify its own limits.

**[F3]** VM exit MUST detach the client's request while a kernel object retains
DMA ownership. Timeout is not completion: unproven shutdown MUST quarantine
buffers until reset or reboot, with bounded accounting that prevents repeated
failures exhausting RAM. **[F4]** Audio MUST use kernel-owned staging buffers.
`src/vm/session_audio.c:cvaudio_codec_prepare/restore` only manages AC-link
transactions/rate state; it does not prove DMA stopped. Its `codec_wait`
iteration timeout MUST become an elapsed-time deadline when ported.

## Input, clocks and NMI

**[F0]** Select input policy before any native i8042 reset, disable command or
port-60 read. `src/boot/input_platform.inc:input_platform_firmware_first`
matches ATI `1002:4C4D`, ESS `125D:1978`, ESS subsystem `0E11:B112`, using
read-only PCI BIOS calls. Preserve that evidence-based firmware-first quirk.
The archived `docs/design/notebook-input-boot-policy.md` inside
`legacy/CiukiOS-docs-legacy-2026-10-09.zip` records unmasked IRQ1/12, an empty
BIOS keyboard queue and status `3C`; these do not identify a command-byte or
SMM defect. The [diskseq39 report](../../dev_diary/2026-10-09-01-report-log-fisici-diskseq39.md)
records working E500 input after the policy change.

**[F0]** Loader-menu input MAY use firmware before handoff. The kernel MUST
support preselected probes without runtime keyboard/mouse service.
**[F1]** Runtime input MUST have exactly one controller consumer: qualified
native driver or qualified firmware backend. Native command/reply sequencing
MUST distinguish keyboard/AUX bytes, ACK/RESEND and unsolicited input, use
deadlines and preserve observed controller state. A timeout MUST disable that
backend, not trigger blind resets. [SeaBIOS PS/2 implementation](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/hw/ps2port.c)
provides bounded, delayed waits; it is not proof of E500 behavior.

**[F1]** USB legacy emulation/SMM ownership MUST be recorded before native
USB takeover. No generic USB-legacy-disable write is allowed. Firmware-first
input MUST retain dependencies until a chipset-specific handoff and physical
test prove replacement input. Firmware and native handlers MUST NOT consume
the same byte. SMM effects remain possible despite kernel ownership; see
[Intel SDM, system management mode](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

**[F0]** PIT channel 0 MUST supply nominal 1000 Hz, mode 2, divisor 1193;
time accounting MUST use the programmed ratio rather than assuming exactly
one millisecond. Channels 1/2 and port `61` MUST NOT be repurposed by arbitrary
drivers. **[F3]** DOS defaults to virtual divisor 65536, approximately
18.2 Hz, and guest programming MUST NOT change the host PIT.
[Linux PIT implementation](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/clocksource/i8253.c).

**[F0]** RTC reads MUST serialize index/data access, preserve the software
NMI-mask policy, validate UIP/seconds consistently and handle BCD/binary and
12/24-hour modes with a timeout. CMOS configuration writes are forbidden.
IRQ8 MUST stay masked unless its service owns enabling and clearing register-C
causes. [Linux MC146818 access](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c).
**[F0]** NMI MUST use an allocation-free, lock-free diagnostic path; unknown
NMI causes halt with bounded output, without disk access. Never mask NMI as a
normal lock. `src/com/vbe_fb.inc:vc_fb_begin/end/limits` uses temporary CPU
mode/IDT changes for legacy framebuffer bursts; the new kernel MUST replace
these with permanent protected mappings, not port the burst mechanism.

## Firmware-call boundary

**[F0]** The loader MUST finish BIOS disk loading, E820 collection and VBE
mode selection before handoff. The kernel MUST consume copied, validated
`ciuki_boot_info` data. Missing usable LFB means text/serial fallback, not a
runtime BIOS dependency. [VBE 3.0, functions 00h–02h](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/vbe3.pdf).

**[F1, before any runtime firmware]** Calls MUST execute in one serialized
V86 BIOS VM with private stack, IVT/BDA working state, bounded buffers and
explicit mappings of required reserved firmware memory. Ordinary DOS VMs MUST
NOT invoke host firmware. IOPL stays zero; trapped I/O, CLI/STI and PIC/PIT
access MUST preserve host preemption and physical EOI ownership. Firmware IRQ
reflection uses virtual vectors; it MUST NOT remap the physical PIC. Privileged
mode-switching firmware is unsupported unless separately mediated and qualified.
[Intel V86 protection](https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf).

**[F1]** A call MUST acquire a firmware mutex and affected device leases,
stop submissions, fence DMA, save documented state and quiesce relevant driver
callbacks before entry. The registry lock MUST NOT remain held while firmware
executes. Firmware input is a persistent controller lease with its own
serialized event queue; native consumers remain suspended. Unknown firmware
dependencies MUST prevent enablement.

| Earliest phase | Runtime allowlist and required ownership |
| --- | --- |
| F1 | Qualified nonblocking INT16 and INT15/C2 input services plus their firmware IRQ handlers, only for the firmware-owned input backend |
| F1 | INT13 only for a separately qualified firmware-only disk backend selected before native command issuance; no mixing on its controller |
| F4 | Explicit VBE `4F02` mode change only after compositor/accelerator stop and framebuffer mapping revocation; query and validate the new mode before remapping |
| F4+ | APM INT15/53 installation/connect/version, event/status and qualified power-off operations under the exclusive power service |
| F0 onward | No post-handoff E820/E801/AH88 memory discovery, physical A20 control, INT15/86 waits, generic INT15 forwarding, PCI BIOS writes or option-ROM initialization |

**[F1]** A command already issued through native ATA MUST never be retried
through BIOS, including IDENTIFY. Preserve
`src/vm/session_disk_ata.h:cvata_transfer` and
`session_disk_ata.c:issue_identify` quarantine semantics; reconcile retry/error
handling with `vfs-storage-contract.md`.

**[F1]** Initial firmware deadlines MUST be 100 ms for status/input operations,
500 ms for input setup, and 2 s for qualified disk/video/power operations.
Profiles MAY tighten them; increases require recorded evidence. A preemptible
timeout MUST abort the request, quarantine affected devices and permit recovery
only after a proven reset. No infinite retry or assumed register restoration.
The V86 monitor cannot interrupt SMM or bound a hung hardware transaction:
these failures require an external runner timeout and reset. A runtime BIOS
call is never a hard real-time guarantee.

## Power, target qualification and safe mode

**[F0–F4]** Preserve firmware power/thermal policy; do not issue ACPI_ENABLE,
connect APM, disable SMI globally or program undocumented EC/fan registers.
Record discoverable tables, checksums and initial SCI_EN state without taking
ownership. An already-enabled ACPI system MUST NOT be assumed to retain
firmware thermal service; unresolved ownership blocks workload qualification.
Suspend/hibernate remain disabled through F4. **[F4+, if enabled]** Exactly one
power backend MUST own events. ACPI takeover requires validated tables, AML/EC
support, SCI routing, fixed/GPE handlers and thermal/battery/lid methods before
enabling events. ACPI NVS remains reserved. [ACPI software model](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html)
and [thermal control](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/11_Thermal_Management/thermal-control.html).

**[F4+]** APM MAY be the exclusive alternative only after per-target negotiation
and testing. Its worker MUST poll events and promptly reject ordinary suspend
requests; pending processing needs notifications at least every five seconds.
Critical suspend is not rejectable by that protocol: the backend MUST have an
emergency quiescence policy before connection. Battery/lid status MUST remain
unknown when unsupported; no invented percentages or GPIO guesses.
[APM 1.2, sections 4.3 and 4.6](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF).

| Target | Evidence and mandatory policy |
| --- | --- |
| QEMU/SeaBIOS, `pentium3` | **[F0]** Record exact machine version, devices, firmware and 128 MiB profile. Emulation qualifies that profile, not notebook SMM, audio or routing. |
| ThinkPad T23 | **[F0]** Match PCI inventory to expected S3 `5333:8C2E` and Intel ICH3-M; record AC97 BARs/IRQ. **[F4]** Probe the codec only under an audio lease. S3 acceleration stays off: diskseq39 reports copy qualification failure at stage 10. Firmware-driven low-battery transitions appear in the [IBM manual](https://thinkpads.com/support/hmm/hmm_pdf/62p9631.pdf); lid/thermal behavior requires unit-specific tests. |
| Armada E500 | **[F0]** Preserve the exact input quirk above; ATI `1002:4C4D` and ESS `125D:1978` identify expected devices, not successful native drivers. **[F4]** ESS requires its own engine backend, not Intel AC97 DMA programming. The [Compaq guide](https://h10032.www1.hp.com/ctg/Manual/c01129749.pdf) does not establish this unit's ACPI tables or SCI/thermal ownership. |

**[F0]** Safe mode MUST select only qualified console, timer and required
input/storage paths, exposing a reason for each disabled device. Optional
audio, acceleration and firmware calls remain off. **[F4]** GPU qualification
MUST prove idle, save/restore and private-VRAM correctness before enablement.
Retain the principle in `src/vm/session_gpu_savage.c:cvsavage_release` and
`session_gpu_mach64.c:cvmach64_release`: uncertain engine shutdown retains
ownership and forbids CPU framebuffer fallback until safe.

## Latency budgets

**[F0]** These are initial engineering limits, not measured notebook facts:
ordinary IRQ-disabled sections SHOULD take at most 50 microseconds and MUST
finish within 250 microseconds; aggregate hard-IRQ work MUST fit the latter
limit. Driver polls, allocation, firmware and disk logging MUST run outside
these sections. **[F1]** Deferred work SHOULD yield within 1 ms. Budget
violations MUST record site, duration and pending work, disabling an optional
offending backend after safe quiescence.

**[F0]** Timing probes MUST separate observed elapsed stalls from measured
software critical sections; unexplained stalls MUST NOT be labeled SMM without
evidence. TSC use requires CPUID support and calibration against PIT, with
recalibration or rejection on frequency changes. PIT latching cannot reconstruct
arbitrarily many missed wraps. Timer-entry gaps over 2 ms MUST be reported;
qualification MUST resolve whether software, firmware or emulation caused them.

## Acceptance tests

All rows are required at their stated phase, before associated driver enablement.
Use [the canonical runner](test-architecture.md): T0 host tests first, T1 static
checks, T2 smoke, T3 focused suites, T4 hardware. No rebuilds or image copies;
QEMU uses serialized capped runs, fw_cfg selection and disposable qcow2
overlays. Physical T23/E500 tests use the same image/probes via boot menu or
serial, recording image SHA-256, BIOS identity, PCI inventory and outcome.
Unsupported optional devices pass the disablement check, not driver qualification.

| Phase/tier | Concrete pass condition |
| --- | --- |
| F0/T0 | Overlap, wraparound, unknown-size and stale-generation claims fail without register writes; repeated claim/release restores counts. Inject real/spurious IRQ7/15 and verify the exact handler/EOI sequences. |
| F0/T1–T4 | The `boot` matrix of `f0-acceptance.md` (five cold starts and five restarts per target, including QEMU at 128 MiB), preserve registry reservations and reach selected probes. Safe mode and missing serial/LFB paths terminate visibly; optional engines remain inactive. |
| F0/T3–T4 | During 10,000 timer-driven switches, record critical-section maxima and every timer gap above 2 ms; no unclassified budget violation, lost ownership or starvation. User PIC/DMA I/O faults locally. |
| F1/T0,T3 | Model two shared IRQ owners, simultaneous assertions, removal and a stuck source. Both receive service; masking/quarantine is bounded. T4 validates actual routing with a qualified device pair, or records shared hardware coverage as pending. |
| F1/T3–T4 | Input: 100 key make/break cycles with mouse motion, no duplicate/stuck events; E500 additionally repeats firmware-first boot selection without native takeover. Inject stalled replies and verify bounded failure and surviving timer. |
| F1/T0,T3 | Inject BIOS overrun, disallowed I/O and native ATA post-command failure: no host PIC mutation, no BIOS disk retry, retained quarantine. T4 runs only allowlisted calls and verifies device state afterward. |
| F3/T0,T3–T4 | Boundary-crossing DMA requests split/reject correctly. Tear down 100 VMs during pending transfers: freed-page canaries remain intact; fault injection proves failed shutdown retains pinned buffers. Guest CLI/EOI cannot stop host ticks. |
| F4/T3–T4 | Per-driver start/stop/fault loops, audio continuity and private-VRAM fill/copy comparisons pass on each claimed physical target; a timeout never enables competing CPU access. |
| F4+/T3–T4 | Replay power events, test AC/battery status and supervised lid events without initiating suspend; verify one backend, event acknowledgement, thermal ownership and bounded firmware calls. Unknown unit behavior prevents enablement. |

## Open questions

- **[F1] E500 native input failure:** close with matched before/after controller
  command traces, PIC/input snapshots and BIOS/USB-legacy settings on the
  actual unit. Current evidence supports the firmware-first workaround only.
- **[F1/F4+] Physical routing and power ownership:** close separately for T23
  and E500 with PCI revisions/BARs, PIRQ/ELCR observations, ACPI tables/FADT,
  APM installation results and supervised fan/thermal/lid/battery traces.
  Neither the exact SCI route nor autonomous thermal behavior is established.
- **[F4] GPU and audio qualification:** close T23 copy failure, codec/rate
  behavior and E500 ESS/Mach64 support with device-specific register evidence
  and the acceptance runs above. QEMU cannot close these items.
- **[F1–F4] Firmware compatibility envelope:** close each allowlist entry with
  observed memory/I/O dependencies, elapsed times and successful recovery on
  the exact BIOS revision; otherwise leave that entry disabled.

## Interfaces required from other contracts

| Contract | Required interface |
| --- | --- |
| `foundations-transition.md` | **[F0]** Phase alignment, license inventory and retirement of Jemm ownership; qualification records required before enabling migrated drivers. |
| `boot-memory.md` | **[F0]** `ciuki_boot_info` v1 extents, copied firmware/VBE data, reservations, validated platform identity and input-selection handoff; **[F1]** constrained DMA allocation and firmware workspace; **[F4+]** ACPI NVS lifetime. |
| `execution-abi.md` | **[F0]** Interrupt entry/exit, lock ordering, deferred work, monotonic deadlines and generation-safe cleanup; **[F1]** pinned request objects and cancellable BIOS-VM execution. |
| `vfs-storage-contract.md` | **[F1]** Controller ownership, ATA quarantine/no-BIOS-retry, flush/error propagation and storage-ready logging; shutdown never frees active DMA. |
| `dos-dpmi-contract.md` | **[F3]** Virtual PIC/PIT/DMA semantics, private guest buffers, I/O trapping, IRQ injection and VM teardown; host BIOS services remain privileged. |
| `f0-acceptance.md` | **[F0]** Probe identifiers/markers, hardware menu selection, safe-mode tests, timing evidence and runner timeout/reset reporting. |

No change to the shared address-space, interrupt-vector, toolchain or licensing
assumptions is proposed. Nominal 1000 Hz accounts for the PIT's integer divisor.
