.PHONY: help build-floppy build-full build-full-cd verify-full-drivers-payload qemu-test-floppy qemu-test-stage1 qemu-test-full qemu-test-full-stage1 qemu-test-full-doom-taxonomy qemu-test-full-drvload-smoke qemu-test-setup-full-acceptance qemu-test-setup-installer-scenarios qemu-test-all clean

help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy     - build floppy profile scaffold"
	@echo "  make build-full       - build full profile scaffold"
	@echo "  make build-full-cd    - build full-profile bootable CD image"
	@echo "  make verify-full-drivers-payload - verify full-profile driver payload"
	@echo "  make qemu-test-floppy - build + QEMU smoke test (floppy image)"
	@echo "  make qemu-test-stage1 - interactive Stage1 regression (DOS21 + COM/MZ + file I/O)"
	@echo "  make qemu-test-full   - build + QEMU smoke test (full image)"
	@echo "  make qemu-test-full-stage1 - full-profile Stage1 selftest regression"
	@echo "  make qemu-test-full-doom-taxonomy - classify DOOM full-profile taxonomy stages"
	@echo "  make qemu-test-full-drvload-smoke - run full-profile DRVLOAD smoke test"
	@echo "  make qemu-test-setup-full-acceptance - run setup full-profile acceptance test"
	@echo "  make qemu-test-setup-installer-scenarios - run setup installer scenario tests"
	@echo "  make qemu-test-all    - build + QEMU smoke test (floppy + full)"
	@echo "  make clean            - remove build artifacts"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

build-full-cd:
	@bash scripts/build_full_cd.sh

verify-full-drivers-payload:
	@bash scripts/verify_full_drivers_payload.sh

qemu-test-floppy:
	@bash scripts/qemu_test_floppy.sh

qemu-test-stage1:
	@bash scripts/qemu_test_stage1.sh

qemu-test-full:
	@bash scripts/qemu_test_full.sh

qemu-test-full-stage1:
	@bash scripts/qemu_test_full_stage1.sh

qemu-test-full-doom-taxonomy:
	@bash scripts/qemu_test_full_doom_taxonomy.sh

qemu-test-full-drvload-smoke:
	@bash scripts/qemu_test_full_drvload_smoke.sh

qemu-test-setup-full-acceptance:
	@bash scripts/qemu_test_setup_full_acceptance.sh

qemu-test-setup-installer-scenarios:
	@bash scripts/qemu_test_setup_installer_scenarios.sh

qemu-test-all:
	@bash scripts/qemu_test_all.sh

clean:
	@rm -rf build
	@echo "build/ removed"
