.PHONY: help build-floppy build-full build-full-cd build-shell-com build-doom-vanille-probe fetch-costa fetch-network-stack verify-full-drivers-payload verify-phase5-runtime-ownership test-serial-log-normalize qemu-run-full-cd qemu-test-full-cd qemu-test-full-cd-shell-drive qemu-test-full-cd-shell-com-boot qemu-test-full-cd-shell-com-boot-fallback qemu-test-floppy qemu-test-stage1 qemu-test-full qemu-test-full-stage1 qemu-test-full-runtime-probe qemu-test-full-costa qemu-test-full-network-ftp qemu-test-full-network-icmp qemu-test-full-doom-taxonomy qemu-test-full-doom-audio qemu-test-full-doomvan-taxonomy qemu-test-full-doomvan-memory qemu-test-full-doomvan-audio qemu-test-full-doomvan-performance qemu-test-full-opl-audio qemu-test-full-dos-audio qemu-test-full-video-restore qemu-test-full-doomsfx qemu-test-full-doomsfx-dsdoropn qemu-test-full-dos-taxonomy qemu-test-full-wolf3d-taxonomy qemu-test-full-wolf3d-audio qemu-test-full-cutemouse qemu-test-full-drvload-smoke qemu-test-full-shell-stability qemu-test-full-shell-com qemu-test-full-shell-com-boot qemu-test-full-shell-com-boot-fallback qemu-test-release-sweep qemu-test-full-dos-compat-smoke qemu-test-setup-full-acceptance qemu-test-setup-installer-scenarios qemu-test-setup-hdd-install qemu-test-setup-cd-hdd-probe qemu-test-setup-runtime-hdd-install qemu-test-all clean


help:
	@echo "CiukiOS Legacy v2"
	@echo "  make build-floppy     - build floppy profile scaffold"
	@echo "  make build-full       - build full profile scaffold"
	@echo "  make build-full-cd    - build full-profile bootable CD image"
	@echo "  make build-shell-com  - assemble the external SHELL.COM prototype"
	@echo "  make build-doom-vanille-probe - probe external doom-vanille build"
	@echo "  make fetch-costa       - download and verify the official Costa v1.8.0 release"
	@echo "  make fetch-network-stack - download and verify mTCP plus Crynwr packet drivers"
	@echo "  make qemu-run-full-cd - boot the Live/install CD in visual QEMU"
	@echo "  make qemu-test-full-cd - smoke test the Live/install CD D: prompt"
	@echo "  make qemu-test-full-cd-shell-drive - validate Live CD shell drive/CWD commands"
	@echo "  make qemu-test-full-cd-shell-com-boot - validate full-CD SHELL.COM plus COM/MZ/runtime ownership on D:"
	@echo "  make qemu-test-full-cd-shell-com-boot-fallback - omit SHELL.COM and verify loader-fatal halt (legacy target name)"
	@echo "  make verify-full-drivers-payload - verify full-profile driver payload"
	@echo "  make qemu-test-floppy - build + QEMU floppy loader-scaffold smoke test"
	@echo "  make qemu-test-stage1 - FAT12 Stage1 loader-scaffold regression"
	@echo "  make qemu-test-full   - build + QEMU smoke test (full image)"
	@echo "  make qemu-test-full-stage1 - legacy alias for external shell/runtime regression"
	@echo "  make verify-phase5-runtime-ownership - verify loader/kernel boundary and ABI2 image"
	@echo "  make qemu-test-full-runtime-probe - validate runtime ABI and the loader rejection matrix"
	@echo "  make qemu-test-full-costa - fetch, build and validate the Costa desktop graphically"
	@echo "  make qemu-test-full-network-ftp - validate Internet ping plus bidirectional FTP"
	@echo "  make qemu-test-full-network-icmp - validate NETCFG and resident ICMP without FTP"
	@echo "  make qemu-test-full-doom-taxonomy - legacy DOOM taxonomy alias (compat)"
	@echo "  make qemu-test-full-doom-audio - validate original Doom gameplay plus OPL2 music/PC-speaker SFX"
	@echo "  make qemu-test-full-doomvan-taxonomy - isolated doom-vanille startup taxonomy"
	@echo "  make qemu-test-full-doomvan-memory - require doom-vanille to pass its 256 KiB low-DOS allocation"
	@echo "  make qemu-test-full-doomvan-audio - validate doom-vanille OPL2 music plus SB16 SFX"
	@echo "  make qemu-test-full-doomvan-performance - require real-time doom-vanille VGA rendering"
	@echo "  make qemu-test-full-opl-audio - isolate and validate external AdLib/OPL2 music"
	@echo "  make qemu-test-full-dos-audio - validate real-mode, DOS/4GW, OPL2 and SB16 playback"
	@echo "  make qemu-test-full-video-restore - require clean VGA text mode after a graphics child exits"
	@echo "  make qemu-test-full-doomsfx - controlled DOOM WAD SB16 SFX harness"
	@echo "  make qemu-test-full-doomsfx-dsdoropn - controlled DOOM door-open SB16 SFX harness"
	@echo "  make qemu-test-full-dos-taxonomy - classify generic DOS full-profile taxonomy stages"
	@echo "  make qemu-test-full-wolf3d-taxonomy - validate WOLF3D transfer/runtime/video stages"
	@echo "  make qemu-test-full-wolf3d-audio - validate protected Wolf3D AdLib/SB audio over PCI AC97"
	@echo "  make qemu-test-full-cutemouse - external CuteMouse install/INT33/unload workflow on an isolated image"
	@echo "  make qemu-test-full-drvload-smoke - run full-profile DRVLOAD smoke test"
	@echo "  make qemu-test-full-shell-stability - run full-profile shell stability test"
	@echo "  make qemu-test-full-shell-com - run focused external SHELL.COM validation"
	@echo "  make qemu-test-full-shell-com-boot - validate the default full-profile boot into external SHELL.COM"
	@echo "  make qemu-test-full-shell-com-boot-fallback - remove SHELL.COM and verify loader-fatal halt (legacy target name)"
	@echo "  make qemu-test-release-sweep - manual/expensive cross-area release sweep (includes optional doom-vanille memory gate)"
	@echo "  make qemu-test-full-dos-compat-smoke - run full-profile DOS compatibility smoke test"
	@echo "  make qemu-test-setup-full-acceptance - run setup full-profile acceptance test"
	@echo "  make qemu-test-setup-installer-scenarios - run setup installer scenario tests"
	@echo "  make qemu-test-setup-hdd-install - create and boot a disposable full-profile HDD install image"
	@echo "  make qemu-test-setup-cd-hdd-probe - boot direct CD with a blank disposable HDD attached"
	@echo "  make qemu-test-setup-runtime-hdd-install - install from direct CD to disposable HDD via SETUP.COM"
	@echo "  make test-serial-log-normalize - validate single/double/triple serial normalization"
	@echo "  make qemu-test-all    - focused full/full-CD, DOS, shell and runtime ownership gate"
	@echo "  make clean            - remove build artifacts"

build-floppy:
	@bash scripts/build_floppy.sh

build-full:
	@bash scripts/build_full.sh

build-full-cd:
	@bash scripts/build_full_cd.sh

build-shell-com:
	@bash scripts/build_shell_com.sh

build-doom-vanille-probe:
	@bash scripts/build_doom_vanille_probe.sh

fetch-costa:
	@bash scripts/fetch_costa.sh

fetch-network-stack:
	@bash scripts/fetch_network_stack.sh

verify-full-drivers-payload:
	@bash scripts/verify_full_drivers_payload.sh

test-serial-log-normalize:
	@scripts/serial_log_normalize.py --self-test

qemu-run-full-cd:
	@bash scripts/qemu_run_full_cd.sh

qemu-test-full-cd:
	@bash scripts/qemu_run_full_cd.sh --test

qemu-test-full-cd-shell-drive:
	@bash scripts/qemu_test_full_cd_shell_drive.sh

qemu-test-full-cd-shell-com-boot:
	@bash scripts/qemu_test_full_cd_shell_com_boot.sh

qemu-test-full-cd-shell-com-boot-fallback:
	@FULL_CD_SHELL_COM_BOOT_EXPECT_FALLBACK=1 bash scripts/qemu_test_full_cd_shell_com_boot.sh

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

qemu-test-full-costa:
	@bash scripts/qemu_test_full_costa.sh

qemu-test-full-network-ftp:
	@bash scripts/qemu_test_full_network_ftp.sh

qemu-test-full-network-icmp:
	@bash scripts/qemu_test_full_network_icmp.sh

verify-phase5-runtime-ownership:
	@bash scripts/verify_phase5_runtime_ownership.sh

qemu-test-full-doom-taxonomy:
	@DOS_TAXONOMY_USE_CASE=doom DOS_TAXONOMY_PROFILE=dosapp DOS_TAXONOMY_MIN_STAGE=visual_gameplay DOS_TAXONOMY_VISUAL_PROFILE=doom_vga_gameplay DOS_TAXONOMY_DISPLAY_MODE=nographic DOS_TAXONOMY_RUN_COMMAND='run DOOM.EXE' DOS_TAXONOMY_POST_LAUNCH_KEYS='esc ret ret ret' DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=20 DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=2 DOS_TAXONOMY_SCREENSHOT=build/full/qemu-full-doom-taxonomy.ppm DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=10 DOS_TAXONOMY_OBSERVE_SEC=30 QEMU_AUDIO_MODE=on DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_TIMEOUT_SEC=320 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-doom-audio:
	@bash scripts/qemu_test_full_doom_audio.sh

qemu-test-full-doomvan-taxonomy:
	@DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dosapp DOS_TAXONOMY_MIN_STAGE=visual_gameplay DOS_TAXONOMY_VISUAL_PROFILE=doom_vga_gameplay DOS_TAXONOMY_DISPLAY_MODE=nographic DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS/DOOMVAN DOS_TAXONOMY_APP_BINARY_NAME=PCDOOM.EXE DOS_APP_AUX_PRIMARY=DOOM.WAD DOS_APP_AUX_ALIAS=DOOM.WAD DOS_TAXONOMY_CWD='\APPS\DOOMVAN' DOS_TAXONOMY_RUN_COMMAND='run PCDOOM.EXE' DOS_TAXONOMY_POST_LAUNCH_KEYS='esc ret ret ret' DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=20 DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=2 DOS_TAXONOMY_SCREENSHOT=build/full/qemu-full-doomvan-taxonomy.ppm DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=10 DOS_TAXONOMY_OBSERVE_SEC=25 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=none DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_TIMEOUT_SEC=260 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-doomvan-memory:
	@bash scripts/qemu_test_full_doomvan_memory.sh

qemu-test-full-doomvan-audio:
	@bash scripts/qemu_test_full_doomvan_audio.sh

qemu-test-full-doomvan-performance:
	@bash scripts/qemu_test_full_doomvan_performance.sh

qemu-test-full-opl-audio:
	@bash scripts/qemu_test_full_opl_audio.sh

qemu-test-full-dos-audio:
	@bash scripts/qemu_test_full_dos_audio.sh

qemu-test-full-video-restore:
	@bash scripts/qemu_test_full_video_restore.sh

qemu-test-full-doomsfx:
	@DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=transfer_marker DOS_TAXONOMY_APP_DIR_IN_IMAGE=:: DOS_TAXONOMY_APP_BINARY_NAME=DOOMSFX.EXE DOS_TAXONOMY_CWD='\' DOS_TAXONOMY_RUN_COMMAND="run DOS4GW.EXE DOOMSFX.EXE$(if $(DOOMSFX_LUMP), $(DOOMSFX_LUMP))" DOS_TAXONOMY_APP_RUNTIME_MARKERS='\[DOOMSFX\][[:space:]]+PASS|\[\[DDOOOOMMSSFFXX\]\][[:space:]]+PPAASSSS' DOS_TAXONOMY_RUN_DRVLOAD=0 QEMU_AUDIO_MODE=on QEMU_AUDIO_BACKEND=alsa QEMU_TIMEOUT_SEC=260 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-doomsfx-dsdoropn:
	@DOOMSFX_LUMP=DSDOROPN $(MAKE) qemu-test-full-doomsfx

qemu-test-full-dos-taxonomy:
	@DOS_TAXONOMY_USE_CASE=generic DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=runtime_stable DOS_TAXONOMY_APP_DIR_IN_IMAGE=::APPS DOS_TAXONOMY_APP_BINARY_NAME=CIUKEDIT.COM DOS_TAXONOMY_RUN_COMMAND='run CIUKEDIT.COM MATRIX.TXT' DOS_TAXONOMY_APP_RUNTIME_MARKERS='[CIUKEDIT:BOOT]|[CIUKEDIT:OK]|[{1,2}C{1,2}I{1,2}U{1,2}K{1,2}E{1,2}D{1,2}I{1,2}T{1,2}:{1,2}(B{1,2}O{2,4}T{1,2}|O{1,2}K{1,2})]{1,2}' DOS_TAXONOMY_CWD='APPS' DOS_TAXONOMY_RUN_DRVLOAD=0 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-wolf3d-taxonomy:
	@DOS_TAXONOMY_USE_CASE=wolf3d DOS_TAXONOMY_PROFILE=dos_generic DOS_TAXONOMY_MIN_STAGE=visual_gameplay DOS_TAXONOMY_RUN_DRVLOAD=0 DOS_TAXONOMY_DISPLAY_MODE=none DOS_TAXONOMY_POST_LAUNCH_KEYS='a a ret ret ret ret up ctrl' DOS_TAXONOMY_POST_LAUNCH_KEY_DELAY_SEC=5 DOS_TAXONOMY_POST_LAUNCH_KEY_INTERVAL_SEC=3 DOS_TAXONOMY_SCREENSHOT=build/full/qemu-full-wolf3d-taxonomy.ppm DOS_TAXONOMY_SCREENSHOT_DELAY_SEC=6 DOS_TAXONOMY_OBSERVE_SEC=10 QEMU_ACCEL_MODE=kvm QEMU_TIMEOUT_SEC=180 bash scripts/qemu_test_full_dos_taxonomy.sh

qemu-test-full-wolf3d-audio:
	@bash scripts/qemu_test_full_wolf3d_audio.sh

qemu-test-full-cutemouse:
	@bash scripts/qemu_test_full_cutemouse.sh

qemu-test-full-drvload-smoke:
	@bash scripts/qemu_test_full_drvload_smoke.sh

qemu-test-full-shell-stability:
	@bash scripts/qemu_test_full_shell_stability.sh

qemu-test-full-shell-com:
	@bash scripts/qemu_test_full_shell_com.sh

qemu-test-full-shell-com-boot:
	@SHELL_COM_BOOT_AUTORUN=1 bash scripts/qemu_test_full_shell_com.sh

qemu-test-full-shell-com-boot-fallback:
	@SHELL_COM_BOOT_AUTORUN=1 SHELL_COM_BOOT_EXPECT_FALLBACK=1 bash scripts/qemu_test_full_shell_com.sh

qemu-test-release-sweep:
	@set -e; \
		echo "[release-sweep] manual/expensive release sweep"; \
		echo "[release-sweep] doom-vanille memory lane is optional when its local payload is absent"; \
		echo "[release-sweep] ==== test-serial-log-normalize ===="; \
		$(MAKE) test-serial-log-normalize; \
		echo "[release-sweep] ==== build-full ===="; \
		$(MAKE) build-full; \
		echo "[release-sweep] ==== build-full-cd ===="; \
		$(MAKE) build-full-cd; \
		echo "[release-sweep] ==== qemu-test-full ===="; \
		$(MAKE) qemu-test-full; \
		echo "[release-sweep] ==== qemu-test-full-dos-compat-smoke ===="; \
		$(MAKE) qemu-test-full-dos-compat-smoke; \
		echo "[release-sweep] ==== qemu-test-full-cutemouse ===="; \
		DO_BUILD=0 $(MAKE) qemu-test-full-cutemouse; \
		echo "[release-sweep] ==== qemu-test-full-shell-com ===="; \
		$(MAKE) qemu-test-full-shell-com; \
		echo "[release-sweep] ==== qemu-test-full-shell-com-boot ===="; \
		$(MAKE) qemu-test-full-shell-com-boot; \
		echo "[release-sweep] ==== qemu-test-full-shell-com-boot-fallback ===="; \
		$(MAKE) qemu-test-full-shell-com-boot-fallback; \
		echo "[release-sweep] ==== qemu-test-full-runtime-probe ===="; \
		$(MAKE) qemu-test-full-runtime-probe; \
		echo "[release-sweep] ==== qemu-test-full-cd-shell-drive ===="; \
		$(MAKE) qemu-test-full-cd-shell-drive; \
		echo "[release-sweep] ==== qemu-test-full-cd-shell-com-boot ===="; \
		$(MAKE) qemu-test-full-cd-shell-com-boot; \
		echo "[release-sweep] ==== qemu-test-full-cd-shell-com-boot-fallback ===="; \
		$(MAKE) qemu-test-full-cd-shell-com-boot-fallback; \
		echo "[release-sweep] ==== qemu-test-full-doom-taxonomy ===="; \
		$(MAKE) qemu-test-full-doom-taxonomy; \
		echo "[release-sweep] ==== qemu-test-full-doomvan-memory (optional payload) ===="; \
		DOOMVAN_MEMORY_REQUIRED=0 DO_BUILD=0 $(MAKE) qemu-test-full-doomvan-memory; \
		echo "[release-sweep] ==== qemu-test-full-doomsfx ===="; \
		$(MAKE) qemu-test-full-doomsfx; \
		echo "[release-sweep] ==== DOOMSFX_LUMP=DSDOROPN qemu-test-full-doomsfx ===="; \
		DOOMSFX_LUMP=DSDOROPN $(MAKE) qemu-test-full-doomsfx

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
