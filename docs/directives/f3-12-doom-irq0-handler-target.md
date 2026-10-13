# f3-12 — DOOM on the T23: IRQ0 delivered to 00ff:0020 after the if_restore boundary, never returned

- **Model/effort:** `gpt-6-astra`, `xhigh` (third attempt after f3-10 and f3-11 with `gpt-6.1-sol`). **Worktree:** `../CiukiOS-wt-f3-12` from `main`. Host-testable with the `f3-workloads` replays; the lead runs the T23 capture.

## Evidence (T23, image 24c98799, 2026-10-13; DOOM 1.9 shareware from Programs > Games; identical addresses in the previous capture 3299a24f)

DOS/4GW client in protected mode (cs=01df). The client programs the PIT (`vm_pit`: port 43 = 36h, reload 8522 → 140 Hz), then:

```
group=vm_if  n=1 restore=0 matched=1 cs=01df eip=00247f15 vif=0 armed=1 seed=1 seed_cs=01df seed_ip=00248205
group=vm_db  n=1 pm=1 cs=01df eip=00247fac dr6=ffff4ff0 tf=1 if=1 vif=0 armed=1 step=pm_flags
group=vm_if  n=2 restore=1 matched=1 cs=01df eip=00247fac vif=1 armed=0
[dosvm] event=pic_stall vm=1 gen=7232 pic=0 irq=0 ms=1002 cs=01df eip=002483e2 isr=01 irr=01 imr=f8 vec=08 to=00ff:00000020 boundary=if_restore at=01df:00247fac pm=1 depth=0 ae=0 sm=0
group=vm_gate … cs=01df eip=002483e2 if=1 vif=1 armed=0 shadow=0 irr=01 isr=01 imr=f8   (every 250 ticks, forever)
group=vm_clock … edges grow, delivered frozen at 656
```

Before the stall the `vm_if` records alternate between 2482d9 and 2465b2 with `seed_ip` 24821d/2482da (n=6…13), i.e. the client's own CLI/STI pairs around its timer setup. The last IRQ0 was delivered (`vec=08`) to **00ff:00000020** right after the `if_restore` boundary at 01df:247fac; the handler frame never returned (no IRET, no EOI), ISR0 stays set, the client spins at 2483dc/2483e2 waiting for the tick count. Earlier IRQ0 deliveries (656 of them during startup) returned normally. f3-11 made the delivery transactional (ISR set only with a constructed frame) and added the `pic_stall` record; the host replay of the exact sequence passed before and after, so the host model differs from the hardware here.

## Task

1. Establish what 00ff:0020 is at that moment: the client's installed PM handler for vector 08 (DPMI 0205 records in the log show vector 08 installed at sel 0087 off 6d70 during startup, then later changes?), the DPMI host's default reflection stub, or a stale/garbage target (an LDT selector 00ff never allocated, a freed selector, a descriptor whose base changed). Reconstruct from the DPMI selector allocation records and the vector table; add a verbose record on every `set PM vector` for vectors 08–0F with the new target and a record when a delivery targets a selector that is not present/valid.
2. Explain why the delivery right after the `if_restore` boundary differs from the 656 earlier ones: the virtual-IF restore path of f3-10 (dpmi `if_armed`, decode seed, single-step `pm_flags`) interleaved with a pending IRQ0 edge; the frame constructed while the restore is still armed; the client's own CLI/STI handling (DOS/4GW switches interrupt handlers with CLI around 2482d9/2465b2). Build a host replay that reproduces the exact hardware interleaving (edge pending at the DB retirement, delivery on the next boundary) — the f3-11 replay did not — and fix at the cause. The virtual IF/VIF model is described in `docs/design/` (F3 contracts) and `dev_diary/2026-10-13-*`: keep it consistent.
3. No automatic EOI, no masking the symptom; a documented, bounded recovery is acceptable only if the reference behaviour (a real 386 with a real PIC) would also recover.
4. `f3-workloads` and the kernel host suites green.

## Acceptance

A host replay that fails before and passes after; the report explains the mechanism with file:line; the lead re-tests DOOM on the T23 (menu, keys, a level, sound later).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f3-12-report.md`.
