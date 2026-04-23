.PHONY: help build-floppy build-full qemu-test-floppy qemu-test-stage1 qemu-test-full qemu-test-all opengem-trace-full opengem-acceptance-full opengem-soak-full opengem-hardware-lane-pack opengem-gate-final opengem-regression-lock opengem-perf-baseline opengem-perf-check clean

help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy     - build floppy profile scaffold"
	@echo "  make build-full       - build full profile scaffold"
	@echo "  make qemu-test-floppy - build + QEMU smoke test (floppy image)"
	@echo "  make qemu-test-stage1 - interactive Stage1 regression (DOS21 + COM/MZ + file I/O)"
	@echo "  make qemu-test-full   - build + QEMU smoke test (full image)"
	@echo "  make qemu-test-all    - build + QEMU smoke test (floppy + full)"
	@echo "  make opengem-trace-full      - full-profile OpenGEM DOS syscall trace artifacts"
	@echo "  make opengem-acceptance-full - OpenGEM graphical acceptance loop with metrics"
	@echo "  make opengem-soak-full       - OpenGEM long-session soak campaign (20-30 min)"
	@echo "  make opengem-hardware-lane-pack - package hardware validation templates"
	@echo "  make opengem-gate-final      - official OG-P0-05 final pass/fail gate"
	@echo "  make opengem-regression-lock - OG-P2-01 historical regression lock checks"
	@echo "  make opengem-perf-baseline   - OG-P2-02 baseline capture for performance budgets"
	@echo "  make opengem-perf-check      - OG-P2-02 periodic budget check against baseline"
	@echo "  make clean            - remove build artifacts"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

qemu-test-floppy:
	@bash scripts/qemu_test_floppy.sh

qemu-test-stage1:
	@bash scripts/qemu_test_stage1.sh

qemu-test-full:
	@bash scripts/qemu_test_full.sh

qemu-test-all:
	@bash scripts/qemu_test_all.sh

opengem-trace-full:
	@bash scripts/opengem_trace_full.sh

opengem-acceptance-full:
	@bash scripts/opengem_acceptance_full.sh

opengem-soak-full:
	@bash scripts/opengem_soak_full.sh

opengem-hardware-lane-pack:
	@bash scripts/opengem_hardware_lane_pack.sh latest

opengem-gate-final:
	@bash scripts/opengem_gate_final.sh

opengem-regression-lock:
	@bash scripts/opengem_regression_lock.sh

opengem-perf-baseline:
	@bash scripts/opengem_perf_baseline.sh

opengem-perf-check:
	@bash scripts/opengem_perf_budget_check.sh

clean:
	@rm -rf build
	@echo "build/ removed"
