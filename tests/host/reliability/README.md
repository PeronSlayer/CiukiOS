# F0-40 regression entry points

The full `scripts/test/host_kernel_tests.sh` gate runs these regressions
sequentially. Run it with `nice -n 19` and `ASAN_OPTIONS=detect_leaks=0`.

`test_loader_progress.py` executes NASM's production progress routines with
ndisasm and a bounded instruction executor, substituting the text sink and
BDA clock. It covers stage tokens, stopped ticks, midnight wrap, retained
failure masks, warm entry and refusal to run a partial ELF. Existing loader
menu/UART tests cover their separate policies and actual UART instructions.
It does not emulate BIOS execution or qualify firmware timing.

Other invariants extend the existing production harnesses: SuperSavage
register/provider restore; shared IRQ member detach; USB root/hub isolation
and lease-loss polling; per-device delayed storage errors and sleeping VFS
waits; audio unproved-stop records; DOS VM stop/cleanup; power raw checkpoints
after FAT shutdown; protected loader-trail publication. The
[audit table](../../../docs/validation/f0-40-reliability-audit.md#invariants-and-failure-domains)
names the exact test/function for each failure and states the remaining
hardware and pre-kernel persistence limits.
