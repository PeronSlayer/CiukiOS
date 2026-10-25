# Review fixes for wt/101-dosvid2

- **Written:** 2026-10-25 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Fix both P1 races of the review below with regression tests that fail before the fix (a copy-back started while an existing leave is waiting; a poll completing during a mode-change copy-back), keep the STRETCH work unchanged, rerun the DOS VM and display/desktop suites you touch, and report (say which sanitizer runs the lead must repeat).

**Verdict: merge after fixes.** HEAD `e1a82992b` has two P1 races in LFB revocation.

1. **An existing leave transaction can restore the provider during copy-back.**  
   [dosvm_vga_vbe.c:235](/home/peronslayer/Desktop/CiukiOS-wt-101-dosvid2/src/kernel/vm/dosvm_vga_vbe.c:235), called from [dosvm_fullscreen.c:382](/home/peronslayer/Desktop/CiukiOS-wt-101-dosvid2/src/kernel/vm/dosvm_fullscreen.c:382).

   A leave can yield in its `guest-revoke` checkpoint before `park()` sets the VM hold. The guest can then enter a mode change and begin copy-back. If that continuation remains unfinished when `park()` times out, the leave calls `dosvm_vbe_direct_revoke()`, which silently returns because `copying` is true. Leave nevertheless proceeds to `display_fullscreen_end()`.

   The provider alias can therefore disappear while the guest continuation still reads it; borrowed PTE restoration is also unfinished. The entry-time `copying` check does not protect a leave already underway.

   **Minimal fix:** distinguish “revocation complete” from “another continuation owns revocation.” Keep the VM and provider pinned, and defer restoration until `pages == 0 && !copying`, including after park timeout. Add a test that starts copy-back while an existing leave is waiting.

2. **Mode change can restore a stale `fullscreen_busy=true` and permanently block return.**  
   [dosvm_fullscreen.c:245](/home/peronslayer/Desktop/CiukiOS-wt-101-dosvid2/src/kernel/vm/dosvm_fullscreen.c:245) and [dosvm_fullscreen.c:256](/home/peronslayer/Desktop/CiukiOS-wt-101-dosvid2/src/kernel/vm/dosvm_fullscreen.c:256).

   `dosvm_fullscreen_poll()` holds `fullscreen_busy` across its first-frame checkpoint, which can sleep. A guest mode change can save that true value and yield during copy-back. The existing poll continuation can then finish and clear the flag. When mode change resumes, it writes its saved true value back.

   Subsequent polls return immediately, leave returns `-EBUSY`, and the held VM cannot finish returning to the desktop.

   **Minimal fix:** give the mode-change continuation its own pin/ownership state instead of saving and restoring a shared boolean. Existing poll/leave continuations must recheck that state after yielding. Test completion of an already-running poll during copy-back.

The STRETCH implementation has **no separate blocker found**. The added register offsets, opcode 2, GO bit, clip selection, disabled Bresenham overrides, formats and submission counts agree with the [Voodoo3 programming guide](https://bitsavers.trailing-edge.com/components/3dfx/Voodoo3_Programming_Guide_r1.4a_199906.pdf). The equal-width, single-source-row restriction supports exact replication; clipping preserves the original rectangle. Capability selection requires the command bit, feature bit and hook, with clean software fallback.

Outside those races, the ordinary UP mapping path preserves retained RAM accounting and saved PTEs, matches provider cache attributes, and routes screenshot/kernel transfers through the lease. Active-address-space invalidation and subsequent CR3 reloads fit the [Intel SDM rules](https://cdrdv2-public.intel.com/825758/253668-sdm-vol-3a.pdf). The two races prevent accepting the overall revocation guarantee.

Validation: `git diff --check` passed. The existing desktop binary exercised all three zoom modes before LeakSanitizer failed at exit. The DOS VM binary stopped at denied COW-fixture creation in this read-only sandbox. The pinned X.Org URLs could not be fetched; register verification used the vendor guide. No files changed.