# f1-41 — Savage on the T23: the ports answer, the lock registers read back with status bits set

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-41` from `main`. Host-testable; the lead runs the T23 capture.

## Evidence (T23, image 24c98799, 2026-10-13, f1-38 merged)

```
[accel] savage step op=pr8 reg=003c3 value=00000001
[accel] savage step op=pw8 reg=003c3 value=00000001
[accel] savage step op=pr8 reg=003cc value=000000ef
[accel] savage step op=pw8 reg=003c2 value=000000ef
[accel] savage step op=pw8 reg=003d4 value=00000038   pw8 003d5 value=00000048
[accel] savage step op=pw8 reg=003d4 value=00000039   pw8 003d5 value=000000a0
[accel] savage step op=pw8 reg=003d4 value=00000038   pr8 003d5 value=00000068
[accel] savage step op=pw8 reg=003d4 value=00000039   pr8 003d5 value=000000a8
[accel] savage quarantined reason=crtc_locked
```

The legacy ports respond (3c3 = 01, MISC = EF). After writing CR38 = 48h the register reads **68h**; after CR39 = A0h it reads **A8h**: the written bits are there plus one extra bit each (CR38 bit 5, CR39 bit 3). `savage2d_start` (`src/kernel/drivers/savage2d.c:280`) requires equality and fails. The X.org and Linux savage drivers never read the lock registers back.

## Task

1. Find in the Savage4/SuperSavage register reference (RE046-B, CR38/CR39 pages) what the extra bits mean (read-only status, reserved bits that read as 1, or the lock-state indication) and cite it in the report and in `docs/design/savage-2d.md`.
2. Replace the equality test with a functional one: after the unlock, read CR40 through the ports, write it with bit 0 set, read back; if bit 0 did not change, the extended registers are still locked (`crtc_locked`); otherwise continue (CR40.0 is the bit the sequence needs anyway). Keep the read-back values in the step records. Apply the same rule to the MMIO alias probe (compare the CR38 value read through MMIO with the one read through the ports, not with the written constant).
3. Review the rest of the sequence for other equality checks against written constants that the hardware may decorate (SR26 pipe select, MISC colour decode, CR66 reset read-backs) and make each tolerant in the way the reference allows; never weaken a check that guards the chip bug of f1-27 (no MM48C60 read before the CR66 reset).
4. `accel_work` took 385 ms on the T23 (`[work] budget exceeded site=accel_work us=385566`): the attempt marker write plus two 10 ms delays plus the polling cannot explain it; measure where the time goes in the host harness and in the step records (add `us=` to the quarantine/ready record) and remove avoidable waits.

## Acceptance

Host tests updated (fake chip returning 68h/A8h after the unlock must pass; a fake that ignores the unlock must fail with `crtc_locked`); i686 kernel compiles. The lead qualifies on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-41-report.md`.
