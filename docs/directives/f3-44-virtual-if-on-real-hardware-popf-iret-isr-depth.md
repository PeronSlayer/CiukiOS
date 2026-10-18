# f3-44 — DOOM slows to a crawl: on real hardware the monitor believes the guest's interrupts are disabled 60 % of the time; IRQ 0 reaches the game at 17 Hz and the SB16 IRQ at 6.6 Hz

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-44` from main (after `06c3d1fa`).
- **Authority:** `src/kernel/vm/dosvm.c` (VIF tracking, `dosvm_deliver_irq`, recovery), `dpmi*.c` (PM IRQ reflection, trampolines, depth), `v86*.c` (CLI/STI/POPF/IRET handling), `dosvm_perf.inc` (records), host CPU seam `tests/host/dosvm_guest_cpu.c` (libx86emu), `tests/host/dosvm_*`, `docs/design/execution-abi.md`, `docs/validation/f3-42-guest-isr-latency.md`.

## Evidence (T23, image 9af46936, `legacy/local/physical/9af46936/BOOT.LOG`, menu L, DOOM windowed then native full screen; read-only)

- `vm_perf_irq … counts=87,1,0,0,0,0,0,33` per 5 s: IRQ 0 delivered 87 times (17 Hz; DOOM programs the PIT at 140 Hz, `vm_beat_pit edges=5353` confirm the timeline) and IRQ 7 33 times (the SB16 block cadence is 86/s). The game runs at one eighth speed later in the level: "lagga male poco più avanti nel primo livello".
- `[vm-audio] cadence … irq_latency_avg_us=17217…20275 merged=237…258/430 if_clear=3017…3180` per 5 s; `vm_heartbeat … pm=1 cs=01df eip=002470a2 if=1 vif=0` and `eip=0024ee29 … vif=0` while executing ordinary game code; later `pm=0 cs=f000 eip=00000102 if=1 vif=0 depth=1` at the DOS prompt: the monitor still counts one outstanding hardware ISR.
- f3-42's host replay (libx86emu) reports 556 µs average latency and `if_clear` ≈ 0 for the same game: the host CPU seam does not reproduce the hardware behaviour.
- Architecture: the DPMI client runs at CPL 3 with IOPL < 3; CLI/STI trap (#GP) and set the virtual IF; **POPF and IRET at CPL 3 with IOPL < 3 do not trap and leave IF unchanged** (Intel SDM Vol. 3, POPF/IRET operation; DPMI 0.9 §2.4.1 warns clients that POPF/IRET may not restore IF). DOOM and its DMX library protect critical sections with `PUSHF; CLI; …; POPF`: every such sequence leaves the virtual IF clear until an explicit STI, so pending IRQs wait for the recovery path, which f3-42 made decline while a hardware ISR is "outstanding" (`depth=1` stays set when the ISR returns through a path other than the host trampoline, e.g. DOS/4GW chaining or a client IRET to a modified frame).

## Required

1. **Host model first.** Make the libx86emu seam honour the real semantics at CPL 3 / IOPL < 3: POPF and IRET leave IF unchanged (and IOPL), STI/CLI raise #GP to the monitor; the DOOM replay must then reproduce the T23 pattern (if_clear > 50 %, IRQ 0 far below 140 Hz) before any fix; record the before numbers.
2. **Virtual IF resync.** Treat the virtual IF as a hint, not a lock: on every monitor entry, derive the guest's intended IF from the evidence available — the interrupted frame's EFLAGS image on the client stack for IRET/POPF frames, the DPMI host state, the TF-after-CLI technique (set TF when a CLI traps so the next instruction boundary reports whether a POPF restored IF; Microsoft's Windows 95 account of exactly this), or a bounded assumption: a client that has run more than 1 ms of ordinary code (no CLI since, outside any ISR) with a pending IRQ gets the IRQ delivered. Document the chosen rule in `execution-abi.md` with the sources.
3. **ISR depth by evidence.** An outstanding hardware ISR record expires when the client's stack pointer rises above the frame the monitor built, when the return address is executed, or after a bounded time (the ISR cannot legitimately last 50 ms); the trampoline remains the fast path. `depth` must be 0 at the DOS prompt.
4. **Acceptance on the corrected host replay:** IRQ 0 ≥ 135/s and IRQ 7 ≥ 80/s during gameplay with sound, `if_clear` ≤ 5 % of the window, average SB IRQ latency ≤ 2 ms, `merged` ≤ 5 %; the Quake/DOS4GW/Wolf3D fixtures still pass; `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
5. Report `build/codex/f3-44-report.md` with before/after tables and the exact replay commands.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No PVI/VME dependency (the kernel refuses them); no guest code patching beyond the existing host-owned trampolines; no physical PIC writes.
