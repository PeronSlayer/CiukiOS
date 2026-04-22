.PHONY: help build-floppy build-full qemu-test-floppy qemu-test-full qemu-test-all clean

help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy     - build floppy profile scaffold"
	@echo "  make build-full       - build full profile scaffold"
	@echo "  make qemu-test-floppy - build + QEMU smoke test (floppy image)"
	@echo "  make qemu-test-full   - build + QEMU smoke test (full image)"
	@echo "  make qemu-test-all    - build + QEMU smoke test (floppy + full)"
	@echo "  make clean            - remove build artifacts"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

qemu-test-floppy:
	@bash scripts/qemu_test_floppy.sh

qemu-test-full:
	@bash scripts/qemu_test_full.sh

qemu-test-all:
	@bash scripts/qemu_test_all.sh

clean:
	@rm -rf build
	@echo "build/ removed"
