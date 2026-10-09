.PHONY: help build-full kernel image test-host qemu-test-full qemu-run-full clean \
        legacy-build-full legacy-build-full-cd legacy-qemu-run-full legacy-qemu-test-full \
        fetch-costa fetch-network-stack

# Heavy builds run in a capped scope (see AGENTS.md).
CAP = systemd-run --user --scope -q -p MemoryMax=3G -p MemorySwapMax=1G --
KERNEL = build/f0/VMM.ELF
IMAGE = build/f0/ciukios.img

help:
	@echo "CiukiOS - Ciuki VMM foundations (F0)"
	@echo "  make build-full      - build the kernel and the canonical FAT32 image ($(IMAGE))"
	@echo "  make kernel          - build only $(KERNEL)"
	@echo "  make image           - build only the image from the existing kernel"
	@echo "  make test-host       - T0 host tests (kernel library, loader statics, runner, fixtures)"
	@echo "  make qemu-test-full  - T2 boot smoke of the built image through the runner"
	@echo "  make qemu-run-full   - boot the image interactively (scripts/run_f0.sh [selector])"
	@echo "  make clean           - remove F0 build outputs and test runs"
	@echo "Legacy 0.8 image (branch legacy-0.8 is canonical for it): legacy-build-full, legacy-build-full-cd,"
	@echo "  legacy-qemu-run-full, legacy-qemu-test-full"
	@echo "Contracts: docs/design/foundations-transition.md, f0-acceptance.md, test-architecture.md"

# The image needs the kernel: keep the order explicit even under make -j.
build-full: kernel
	@$(MAKE) --no-print-directory image

kernel:
	@python3 scripts/build_kernel.py

image:
	@$(CAP) python3 scripts/build_image.py --kernel $(KERNEL) --out $(IMAGE)

test-host:
	@bash scripts/test/host_kernel_tests.sh
	@python3 tests/loader_stub/test_static.py
	@python3 -m unittest discover -s tests/host

qemu-test-full:
	@python3 scripts/test/run.py f0-smoke --image $(IMAGE)

qemu-run-full:
	@bash scripts/run_f0.sh

clean:
	@rm -rf build/f0 build/test-runs build/host
	@echo "F0 outputs removed; build/external, build/downloads, build/tools and build/releases kept"

# ---- 0.8 line (kept buildable until its components are migrated or retired;
# ---- the canonical 0.8 build is branch legacy-0.8) ------------------------

legacy-build-full:
	@$(CAP) bash scripts/build_full.sh

legacy-build-full-cd:
	@$(CAP) bash scripts/build_full_cd.sh

legacy-qemu-run-full:
	@bash scripts/qemu_run_full.sh --no-build

legacy-qemu-test-full:
	@bash scripts/qemu_run_full.sh --test --no-build

fetch-costa:
	@bash scripts/fetch_costa.sh

fetch-network-stack:
	@bash scripts/fetch_network_stack.sh
