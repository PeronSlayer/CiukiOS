.PHONY: help build-full kernel image test-host qemu-test-full qemu-run-full clean \
        legacy-build-full legacy-build-full-cd legacy-qemu-run-full legacy-qemu-test-full \
        fetch-costa fetch-network-stack sdk lua desktop

# Heavy builds run in a capped scope (see AGENTS.md).
CAP = systemd-run --user --scope -q -p MemoryMax=3G -p MemorySwapMax=1G --
KERNEL = build/f0/VMM.ELF
IMAGE = build/f0/ciukios.img
SDK_MANIFEST = build/tools/ciuki-sdk/manifest.json
SDK_ARCHIVE ?= build/downloads/newlib/newlib-4.5.0.20241231.tar.gz
SDK_ARGS ?= --archive $(SDK_ARCHIVE) --jobs 2
SDK_INPUTS = $(shell find sdk -type f ! -path '*/__pycache__/*') \
             scripts/build_sdk.sh scripts/build_kernel.py config/sdk-pins.json \
             config/toolchain.json src/kernel/include/ciuki/abi.h $(wildcard $(SDK_ARCHIVE))
LUA_SOURCE_ARCHIVE ?= build/downloads/newlib/lua-5.4.8.tar.gz
LUA_TESTS_ARCHIVE ?= build/downloads/newlib/lua-5.4.8-tests.tar.gz
LUA_ARGS ?= --source-archive $(LUA_SOURCE_ARCHIVE) --tests-archive $(LUA_TESTS_ARCHIVE) --jobs 2

help:
	@echo "CiukiOS - Ciuki VMM foundations (F0)"
	@echo "  make build-full      - build the kernel and the canonical FAT32 image ($(IMAGE))"
	@echo "  make sdk             - build/check the pinned offline F2 C SDK (SDK_ARGS=...)"
	@echo "  make lua             - build/check pinned Lua and luac (LUA_ARGS=...)"
	@echo "  make desktop         - build/check ring-3 desktop, demo and approved Ciuki portrait"
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
	@$(MAKE) --no-print-directory lua
	@$(MAKE) --no-print-directory desktop
	@$(MAKE) --no-print-directory image

sdk:
	@$(CAP) bash scripts/build_sdk.sh $(SDK_ARGS)

$(SDK_MANIFEST): $(SDK_INPUTS)
	@$(CAP) bash scripts/build_sdk.sh $(SDK_ARGS)

# The Lua recipe checks input/output hashes and rebuilds only when changed.
lua: $(SDK_MANIFEST)
	@$(CAP) python3 apps/lua/build_lua.py $(LUA_ARGS)

# Both recipes validate SDK/source/output hashes before accepting cached output.
desktop: $(SDK_MANIFEST)
	@$(CAP) python3 apps/desktop/build_desktop.py

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
	@rm -rf build/f0 build/apps build/test-runs build/host
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
