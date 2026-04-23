.PHONY: help build-floppy build-full qemu-test-floppy qemu-test-stage1 qemu-test-full qemu-test-all opengem-trace-full opengem-acceptance-full clean

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

clean:
	@rm -rf build
	@echo "build/ removed"
