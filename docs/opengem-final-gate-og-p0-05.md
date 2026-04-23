# OpenGEM Final Gate (OG-P0-05)

Date: 2026-04-24
Scope: single official pass/fail gate for OpenGEM P0 milestone closure.

## Official Command

Run from repository root:

```bash
make opengem-gate-final
```

This command executes the OG-P0-05 gate script:

```bash
bash scripts/opengem_gate_final.sh
```

## What the Gate Runs

1. Full profile smoke gate (`qemu_run_full --test`)
2. OpenGEM trace artifacts (`opengem_trace_full.sh`)
3. OpenGEM acceptance campaign (`opengem_acceptance_full.sh`)

## Default Thresholds

1. Launch success rate >= 90.00
2. Return-to-shell rate >= 95.00
3. Hang count <= derived max hangs from return threshold
4. Smoke gate must pass

With default `RUNS=20` and `RETURN_THRESHOLD=95`, derived `MAX_HANGS=1`.

## Output Artifacts

1. `build/full/opengem-gate-final.latest.report.txt`
2. `build/full/opengem-acceptance-full.latest.report.txt`
3. `build/full/opengem-trace-full.latest.serial.log`
4. `build/full/opengem-trace-full.latest.qemu-int.log`
5. `build/full/opengem-trace-full.latest.int21-summary.txt`

## Useful Overrides

```bash
RUNS=20 QEMU_TIMEOUT_SEC=12 make opengem-gate-final
```

```bash
bash scripts/opengem_gate_final.sh --no-build --skip-smoke --label local
```

```bash
bash scripts/opengem_gate_final.sh --launch-threshold 95 --return-threshold 98 --max-hangs 0
```

## Milestone Exit Rule

OpenGEM P0 is considered closed only when:

1. gate verdict is PASS
2. final report is committed or attached to release evidence
3. no regressions are introduced in existing full-profile smoke flow
