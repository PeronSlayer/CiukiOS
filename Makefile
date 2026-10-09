.PHONY: help build-full build-full-cd build-kernel-module fetch-costa fetch-network-stack verify-full-drivers-payload verify-phase5-runtime-ownership test-serial-log-normalize qemu-run-full qemu-run-full-cd qemu-test-full qemu-test-full-cd clean

# Heavy builds run in a capped scope (see AGENTS.md).
CAP = systemd-run --user --scope -p MemoryMax=3G -p MemorySwapMax=1G --

help:
	@echo "CiukiOS"
	@echo "  make build-full          - build the full HDD image and refresh the Windows portable ZIP"
	@echo "  make build-full-cd       - build the full Live/install CD image"
	@echo "  make build-kernel-module - rebuild only the recorded CiukiDOS kernel"
	@echo "  make fetch-costa         - download and verify Costa v1.8.0"
	@echo "  make fetch-network-stack - download and verify mTCP plus Crynwr packet drivers"
	@echo "  make verify-full-drivers-payload     - check the image's driver payload"
	@echo "  make verify-phase5-runtime-ownership - check the loader/kernel boundary of the built image"
	@echo "  make test-serial-log-normalize       - self-test the serial log normalizer"
	@echo "  make qemu-run-full       - boot the full image in visual QEMU"
	@echo "  make qemu-run-full-cd    - boot the Live/install CD in visual QEMU"
	@echo "  make qemu-test-full      - headless boot smoke test of the built full image"
	@echo "  make qemu-test-full-cd   - headless smoke test of the built CD image"
	@echo "  make clean               - remove build outputs (keeps build/external, downloads, tools)"
	@echo "Test architecture: docs/design/test-architecture.md"

build-full:
	@$(CAP) bash scripts/build_full.sh

build-full-cd:
	@$(CAP) bash scripts/build_full_cd.sh

build-kernel-module:
	@python3 scripts/build_kernel_component.py --rebuild build/full/obj/kernel-build-command.json

fetch-costa:
	@bash scripts/fetch_costa.sh

fetch-network-stack:
	@bash scripts/fetch_network_stack.sh

verify-full-drivers-payload:
	@bash scripts/verify_full_drivers_payload.sh

verify-phase5-runtime-ownership:
	@bash scripts/verify_phase5_runtime_ownership.sh --no-build

test-serial-log-normalize:
	@python3 scripts/serial_log_normalize.py --self-test

qemu-run-full:
	@bash scripts/qemu_run_full.sh --no-build

qemu-run-full-cd:
	@bash scripts/qemu_run_full_cd.sh

qemu-test-full:
	@bash scripts/qemu_run_full.sh --test --no-build

qemu-test-full-cd:
	@bash scripts/qemu_run_full_cd.sh --test

clean:
	@rm -rf build/full build/test-runs
	@echo "build outputs removed; build/external, build/downloads, build/tools and build/releases kept"
