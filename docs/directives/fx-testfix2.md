# fx-testfix2: irq_work_replay audio harness link regression

- **Written:** 2026-10-26 by the lead. Implementer: Codex gpt-6-luna, effort medium. Base: main a5e35dcf2. Test-harness fix only; no production code change unless the harness cannot be fixed otherwise (then explain).

`scripts/test/host_kernel_tests.sh` fails at `python3 -B tests/host/irq_work_replay.py`: the 'audio' replay (irq_work_replay.py:180, `replay(folder,'audio',...)`) links `src/kernel/core/power_quiesce.c`, whose `power_devices_quiesce_terminal` (power_quiesce.c:28) calls `ata_power_quiesce`, added by the shutdown work merged in 9219ff01e (wt/t23-w32hang); the harness does not link the ATA driver: "undefined reference to `ata_power_quiesce'". Fix it the way the harness handles its other out-of-scope kernel dependencies (stub file or wrap list in irq_work_replay.py / tests/host/audio), keeping the replay's instrumentation and assertions unchanged. Use rg to find every other host harness that links power_quiesce.c without the ATA driver and fix those too.

Run with nice -n 19: `python3 -B tests/host/irq_work_replay.py`, `bash tests/host/audio/run.sh`, and any harness you touched. Report to build/codex/fx-testfix2-report.md. Leave changes uncommitted.
