.PHONY: help build-floppy build-full build-full-cd verify-full-drivers-payload qemu-run-full-cd qemu-test-full-cd qemu-test-full-cd-shell-drive qemu-test-floppy qemu-test-stage1 qemu-test-full qemu-test-full-stage1 qemu-test-full-runtime-probe qemu-test-full-doom-taxonomy qemu-test-full-dos-taxonomy qemu-test-full-wolf3d-taxonomy qemu-test-full-drvload-smoke qemu-test-full-shell-stability qemu-test-full-dos-compat-smoke qemu-test-setup-full-acceptance qemu-test-setup-installer-scenarios qemu-test-setup-hdd-install qemu-test-setup-cd-hdd-probe qemu-test-setup-runtime-hdd-install qemu-test-all clean

help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy     - build floppy profile scaffold"
	@echo "  make build-full       - build full profile scaffold"
	@echo "  make build-full-cd    - build full-profile bootable CD image"
	@echo "  make qemu-run-full-cd - boot the Live/install CD in visual QEMU"
	@echo "  make qemu-test-full-cd - smoke test the Live/install CD D: prompt"
	@echo "  make qemu-test-full-cd-shell-drive - validate Live CD shell drive/CWD commands"
	@echo "  make verify-full-drivers-payload - verify full-profile driver payload"
	@echo "  make qemu-test-floppy - build + QEMU smoke test (floppy image)"
	@echo "  make qemu-test-stage1 - interactive Stage1 regression (DOS21 + COM/MZ + file I/O)"
	@echo "  make qemu-test-full   - build + QEMU smoke test (full image)"
	@echo "  make qemu-test-full-stage1 - full-profile Stage1 selftest regression"
	@echo "  make qemu-test-full-runtime-probe - probe runtime load/entry fallback"
	@echo "  make qemu-test-full-doom-taxonomy - legacy DOOM taxonomy alias (compat)"
	@echo "  make qemu-test-full-dos-taxonomy - classify generic DOS full-profile taxonomy stages"
	@echo "  make qemu-test-full-wolf3d-taxonomy - classify WOLF3D transfer/runtime stages"
	@echo "  make qemu-test-full-drvload-smoke - run full-profile DRVLOAD smoke test"
	@echo "  make qemu-test-full-shell-stability - run full-profile shell stability test"
	@echo "  make qemu-test-full-dos-compat-smoke - run full-profile DOS compatibility smoke test"
	@echo "  make qemu-test-setup-full-acceptance - run setup full-profile acceptance test"
	@echo "  make qemu-test-setup-installer-scenarios - run setup installer scenario tests"
	@echo "  make qemu-test-setup-hdd-install - create and boot a disposable full-profile HDD install image"
	@echo "  make qemu-test-setup-cd-hdd-probe - boot direct CD with a blank disposable HDD attached"
	@echo "  make qemu-test-setup-runtime-hdd-install - install from direct CD to disposable HDD via SETUP.COM"
	@echo "  make qemu-test-all    - validate active profiles: full and full-cd"
	@echo "  make clean            - remove build artifacts"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

build-full-cd:
	@bash scripts/build_full_cd.sh

verify-full-drivers-payload:
	@bash scripts/verify_full_drivers_payload.sh

qemu-run-full-cd:
	@bash scripts/qemu_run_full_cd.sh

qemu-test-full-cd:
	@bash scripts/qemu_run_full_cd.sh --test

qemu-test-full-cd-shell-drive:
	@bash scripts/qemu_test_full_cd_shell_drive.sh

qemu-test-floppy:
	@bash scripts/qemu_test_floppy.sh

qemu-test-stage1:
	@bash scripts/qemu_test_stage1.sh

qemu-test-full:
	@bash scripts/qemu_test_full.sh

qemu-test-full-stage1:
	@bash scripts/qemu_test_full_stage1.sh

qemu-test-full-runtime-probe:
	@bash scripts/qemu_test_full_runtime_probe.sh

qemu-test-full-doom-taxonomy:
	@DOS_TAXONOMY_USE_CASE=doom DOS_TAXONOMY_PROFILE=dosapp DOS_TAXONOMY_MIN_STAGE=visual_gameplay DOS_TAXONOMY_DISPLAY_MODE=nographic DOS_TAXONOMY_RUN_COMMAND='run DOOM.EXE -nosfx' DOS_TAXONOMY_SCREENSHOT=build/full/qemu-full-doom-taxonomy.ppm DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=25 DOS_TAXONOMY_OBSERVE_SEC=50 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=alsa DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_TIMEOUT_SEC=320 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-dos-taxonomy:
	@DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=runtime_stable DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS DOS_TAXONOMY_APP_BINARY_NAME=CIUKEDIT.COM DOS_TAXONOMY_RUN_COMMAND='run CIUKEDIT.COM MATRIX.TXT' DOS_TAXONOMY_APP_RUNTIME_MARKERS='[CIUKEDIT:BOOT]|[CIUKEDIT:OK]|[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}(B{1,2}O{2,4}T{1,2}|O{1,2}K{1,2})]{1,2}' DOS_TAXONOMY_CWD='APPS' DOS_TAXONOMY_RUN_DRVLOAD=0 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-wolf3d-taxonomy:
	@DOS_TAXONOMY_USE_CASE=wolf3d DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=transfer_marker DOS_TAXONOMY_RUN_DRVLOAD=0 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-drvload-smoke:
	@bash scripts/qemu_test_full_drvload_smoke.sh

qemu-test-full-shell-stability:
	@bash scripts/qemu_test_full_shell_stability.sh

qemu-test-full-dos-compat-smoke:
	@bash scripts/qemu_test_full_dos_compat_smoke.sh

qemu-test-setup-full-acceptance:
	@bash scripts/qemu_test_setup_full_acceptance.sh

qemu-test-setup-installer-scenarios:
	@bash scripts/qemu_test_setup_installer_scenarios.sh

qemu-test-setup-hdd-install:
	@bash scripts/qemu_test_setup_hdd_install.sh

qemu-test-setup-cd-hdd-probe:
	@bash scripts/qemu_test_setup_cd_hdd_probe.sh

qemu-test-setup-runtime-hdd-install:
	@bash scripts/qemu_test_setup_runtime_hdd_install.sh

qemu-test-all:
	@bash scripts/qemu_test_all.sh

clean:
	@rm -rf build
	@echo "build/ removed"
