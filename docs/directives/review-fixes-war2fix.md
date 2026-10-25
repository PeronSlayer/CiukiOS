# Review fixes for wt/war2fix

- **Written:** 2026-10-25 by the lead. Implementer: Codex gpt-6.1-sol, effort xhigh. Fix both P2 findings of the review below in `src/kernel/vm/dpmi_dispatch.c` (and `dpmi.c` if the saved state changes), each with a host regression test in `tests/host/dosvm_dpmi_test.inc` that fails before the fix (the exact instruction sequences below), keep the selector-40h and disk-builder work unchanged, and rerun: the DOS VM host test (`tests/host/dosvm_test.c` through its runner in `scripts/test/host_kernel_tests.sh`), `tests/host/dos/run_dos_extenders.sh` (DPMI4GW and the Warcraft II demo with and without EMS), `tests/host/dos/run_dos_memory.sh` and the DOOM-shareware counter check the original agent used (233710 traps, 24 page flips, 1806 IRQs, 167359 SB16 units on 1a28f070: the counters must stay identical). The lead reruns the sanitizer builds.

## Review (Codex gpt-6.1-sol xhigh, commit af5ecf11a)

Two **P2 findings** in `af5ecf11a`. No P1 found.

1. **[P2] Cleanup can erase a newly planted TF through a stack alias.**  
   [dpmi_dispatch.c:869](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:869) plants TF before [armed_push:928](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:928) prunes old images. If two different SS selectors alias the same memory, an abandoned image’s slot can be reused by the new PUSHFD. With matching flags, [armed_unplant:909](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:909) recognizes the newly patched bytes as the old image and clears their TF.

   The new owner remains recorded, but its POPFD produces no trap, leaving virtual IF disabled. A subsequent HLT can stall the client.

   **Minimal fix:** prune/unplant old owners before writing the replacement TF, or explicitly protect the replacement’s overlapping linear range. Add an alias-reuse regression test.

2. **[P2] A popped image can survive indefinitely as a false enclosing owner.**  
   [armed_live:895](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:895) checks the current ESP and unchanged bytes, without remembering an earlier observed pop. For example:

   ```asm
   pushfd
   cli
   pop ecx
   and ecx, 200h
   sti
   sub esp, 20h
   pushfd
   cli
   popfd
   nop
   add esp, 20h
   ```

   At STI, ESP has passed the abandoned image, but no pruning occurs. At the second arm, ESP is below it again and its old bytes remain intact, so it is retained. Consuming the inner image leaves `if_armed == 1`; the later ADD does not trap.

   This stale owner suppresses IF recovery stepping at [1128](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:1128) and return tracking at [1239](/home/peronslayer/Desktop/CiukiOS-wt-war2fix/src/kernel/vm/dpmi_dispatch.c:1239). Previously, consuming the second image cleared the single owner.

   **Minimal fix:** prune at observed monitor boundaries, after processing any loaded TF—especially STI—and before ownership suppresses tracking. Add the sequence above as a regression test.

