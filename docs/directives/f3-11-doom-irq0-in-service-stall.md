# f3-11 — DOOM on the T23 stops again: IRQ0 left in service after the return-boundary restore of f3-10

- **Model/effort:** `gpt-6.1-sol`, `xhigh` (second attempt on this problem after f3-10; if it fails the lead escalates to `gpt-6-astra`). **Worktree:** `../CiukiOS-wt-f3-11` from `main` (f9f112a0, f3-10 merged in 854ca7ef). Host-testable with the `f3-workloads` replays.

## Evidence (T23, image 3299a24f, boot `L`, 2026-10-13; DOOM 1.9 shareware from Programs > Games)

The VM boots, DOS/4GW enters protected mode (`vm_vec` records at cs=0087, then the client at cs=01df), the PIT delivers 721 IRQ0 ticks, then:

```
group=vm_if  n=13 restore=0 matched=1 cs=01df eip=00247f15 vif=0 armed=1 seed=1 seed_cs=01df seed_ip=00248205
group=vm_db  n=1  pm=1 cs=01df eip=00247fac dr6=ffff4ff0 cause=ffff4ff0 tf=1 if=1 vif=0 armed=1
group=vm_if  n=14 restore=1 matched=1 cs=01df eip=00247fac vif=1 armed=0 seed=1 seed_cs=01df seed_ip=00247f16
group=vm_clock n=1 tick=176857 clocks=51177314 edges=911 delivered=721 reload=8522
group=vm_gate  n=1 pm=1 cs=01df eip=002483e2 if=1 vif=1 armed=0 shadow=0 irr=01 isr=01 imr=f8
… every 250 ticks the same: eip alternates 002483dc/002483e2, irr=01 isr=01, edges 946 → 1226, delivered stays 721
```

From the restore at 247fac onward **IRQ0 stays in service** (`isr=01`, a second IRQ0 pending in `irr`), no EOI ever arrives, `delivered` freezes at 721 while PIT edges keep coming, and the client spins in a two-instruction loop at 2483dc/2483e2 (waiting for the tick counter, I_GetTime-like). On the screen: DOOM shows nothing usable ("non parte" for the owner).

Also to explain: 41 `vm_db` records with `tf=1 … armed=0`, starting at the VM's first instruction (`n=1 pm=0 cs=0000 eip=00007c51 tf=1 armed=0`) and throughout real mode (cs=0800, 0300, 132e, 13d7, 1753): single-step traps while nothing is armed. Either the monitor traces the boundaries by design (then record it as such) or TF leaks into the guest (the 2026-07 lesson in the project history: a `popf` loading a saved register into FLAGS). A permanently single-stepped VM would also explain 20 s to reach the menu.

## Task

1. Root cause of the in-service stall at the f3-10 return boundary: which of (a) the client's EOI write (`out 20h,20h`, emulated in `dosvm_pic.c`) swallowed while the boundary state was armed/shadowed, (b) an IRQ0 injected (ISR set, `delivered` counted) whose frame was discarded when the DB restore rewrote the client state, so the handler never ran, (c) EOI accounted to the wrong PIC state (auto-EOI, specific EOI, mask); reproduce with a host replay (`f3-workloads`, the DOOM replay harness) that forces the same sequence (arm, DB at the restore address, IRQ0 edge in the same window), then fix at the cause. Any IRQ injection must leave ISR consistent with the frame actually delivered.
2. The `vm_db armed=0` records: prove whether TF is set in the guest by design or by leak; if a leak, fix it and add a host test on the flags path; if by design, bound the cost and document.
3. Diagnostics: a `[dosvm]` verbose record when a PIC in-service bit stays set for more than 1 s without EOI (CS:EIP, ISR, IRR, IMR, the last injected vector and the last boundary event) — diagnostic only, no automatic EOI.
4. `f3-workloads` and the kernel host suites stay green.

## Acceptance

Host replay reproducing the stall fails before and passes after; report the exact mechanism with file:line. The lead re-tests DOOM on the T23 (menu, keys, a level).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f3-11-report.md`.
