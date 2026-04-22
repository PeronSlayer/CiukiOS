.PHONY: help build-floppy build-full clean

help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy   - genera artefatto base profilo floppy"
	@echo "  make build-full     - genera artefatto base profilo full"
	@echo "  make clean          - pulizia directory build"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

clean:
	@rm -rf build
	@echo "build/ rimossa"
