# f1-24 — Write-combining for the linear framebuffer (MTRR) and repaint cost measurement

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-24` (branch `wt/f1-24` from `main`). Host-testable (MTRR range computation, MSR programming model in the kernel host harness); the lead measures on the T23 and E500.

## Owner request (2026-10-12)

The desktop on the T23 is not responsive enough. Software rendering writes the VBE linear framebuffer through the AGP/PCI aperture; without write-combining every store is an uncached bus transaction (tens of MB/s). Pentium III and later CPUs support MTRRs (Intel SDM vol. 3 §12.11): a variable-range MTRR of type WC over the framebuffer makes the compositor's stores burst (hundreds of MB/s), the single largest 2D speed-up available without a GPU driver.

## Scope

1. Kernel `src/kernel/core` (or drivers): at framebuffer activation, if CPUID reports MTRR (EDX bit 12) and the fixed/variable MTRRs are enabled, program a free variable-range MTRR pair (base = LFB physical base, size = the smallest power of two covering pitch × height, aligned, not overlapping existing ranges; split into two ranges if needed) with type WC (1), following the SDM sequence (disable interrupts, disable caches, WBINVD, flush TLB, write MSRs, re-enable). Record `[display] mtrr wc base=.. size=.. result=..` on the serial log. Also set the PAT or page attribute of the kernel mapping of the LFB to WC if the kernel maps it (PWT/PCD bits consistent with the MTRR type: WC via MTRR needs the PTE as cacheable or WC through PAT; document the combination used and why).
2. Safety: never touch MTRRs under a hypervisor without MTRR support (QEMU TCG exposes MTRR: verify it behaves, otherwise skip with a log line); restore nothing at shutdown (BIOS reinitialises); refuse when no free variable MTRR exists; the display grant for DOS VMs keeps working (VGA aperture A0000h stays UC).
3. Measurement: a kernel/desktop counter of microseconds spent in full-screen presents (the compositor's copy to the LFB) exposed in the Task Manager "System" line or a serial record in verbose boot (`L`), so the lead can compare before/after on the T23.

## Tests (host)

MTRR range arithmetic (bases, sizes, splits, overlap with BIOS ranges parsed from synthetic MSR state), MSR write sequence order in the host harness, CPUID gating, QEMU detection path.

## Constraints

clang i686 `-Werror`, no QEMU, no image builds, no commits, no diary, `nice -n 19`. Report to `build/codex/f1-24-report.md` with references (Intel SDM sections) and the exact MSR sequence.
