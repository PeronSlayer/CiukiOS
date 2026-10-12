# F1-24 framebuffer write combining

Implementation of `docs/directives/f1-24-framebuffer-write-combining.md`.
QEMU and physical qualification belong to the lead.

## Research and decision (2026-10-12)

Intel [SDM Vol. 3A, 253668-084US](https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf),
§12.11.1–4 defines capability checks, physical-address masks and aligned
power-of-two ranges; §12.11.7.2–8 gives the cache/TLB update procedure.
§12.5.2.2, Table 12-7, and §12.12 describe PAT/MTRR composition: WC with
page WB gives WC, while page UC overrides WC. §12.3.1 and §9.2.5 cover
draining buffered writes with locked instructions. The implementation uses
MTRR WC and PAT index zero (verified WB), without changing PAT. The
pre-PAT path clears PCD/PWT. Missing extended CPUID uses the MTRR-specific
36-bit fallback in §12.11.2.3.

QEMU [v10.1.0 MSR helpers](https://github.com/qemu/qemu/blob/v10.1.0/target/i386/tcg/system/misc_helper.c)
implement the eight variable pairs, DEF_TYPE and MTRRCAP, including WC.
Its [CPUID implementation](https://github.com/qemu/qemu/blob/v10.1.0/target/i386/cpu.c)
exposes `TCGTCGTCGTCG` in leaf 40000000h. This supports attempting the
same sequence when MTRR/MSR are advertised, with MSR readback checked.
[TCG documentation](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html#memory-control-and-maintenance)
says cache behaviour is not modeled; a successful emulated update cannot
establish a physical speed improvement.

Repository inspection found two independent UC aliases: the early console
and `fbdev_init` both called `vmm_map_mmio(..., true)`. The RAM direct map
already excludes the LFB. Both callers will use `vmm_map_framebuffer`, which
configures WC before creating the first alias and retains its decision for
the immutable boot framebuffer. Generic MMIO mappings keep their existing
attributes. No framebuffer PTE exists during the MTRR update.

## Range and fallback policy

Use only invalid variable pairs; leave valid BIOS pairs and fixed MSRs intact.
First try the smallest power of two covering the frame at its physical base.
If alignment or overlap prevents that, try two adjacent aligned ranges, with
the final range rounded up. Refuse a request needing more than two pairs.
Check the complete rounded extent against every active BIOS range and E820
RAM/ACPI/NVS entry. Malformed active BIOS masks cause refusal.

Never round below the LFB base. The entire extent must fit within VBE's
reported video memory from that base; if unavailable, permit only the
page-rounded frame. Reject bases below 1 MiB, non-page-aligned bases,
overflow, unsupported WC, disabled fixed/variable MTRRs, unavailable pairs,
non-WB PAT entry zero or non-UC PAT entry three. The legacy VGA aperture
retains UC PTEs, including those used by DOS display grants. No shutdown
restoration is added. This procedure is for the existing UP kernel only;
starting additional CPUs would require the SDM rendezvous protocol.

## Measurement

Successful full-screen copies to the live framebuffer are timed with serialized
TSC reads, using the existing PIT calibration. Drain WC writes before the final
timestamp and before releasing console/presenter ownership. Verbose (`L`)
boot emits the first timed copy and then at most one record per second, with
sample count, cumulative/last/maximum microseconds and TSC kHz. Partial,
failed, fill and fixture copies are excluded. Copy time includes preemption;
mutex wait, composition and serial output are outside the measured interval.
Without calibrated TSC, no microsecond result is claimed.
Intel [SDM instruction reference, RDTSC](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf)
requires ordering against both earlier and later instructions. Surround each
read with CPUID for the P6 baseline; the reported interval includes boundary
serialization overhead. Older hardware can change TSC rate with power state:
the existing boot calibration is a comparison aid, not an invariant clock.
