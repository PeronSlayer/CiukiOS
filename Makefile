PROJECT := CiukiOS

CC := clang
LD := ld.lld
AS := clang

COMMON_CFLAGS := -std=c11 \
                 -ffreestanding \
                 -fno-stack-protector \
                 -fno-pic \
                 -fno-builtin \
                 -m64 \
                 -mno-red-zone \
                 -mcmodel=small \
                 -Wall \
                 -Wextra

ASFLAGS := -target x86_64-unknown-none-elf -c
KERNEL_CFLAGS := $(COMMON_CFLAGS) -Ikernel/include
STAGE2_CFLAGS := $(COMMON_CFLAGS) -Istage2/include -Iboot/proto
KERNEL_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T kernel/linker.ld
STAGE2_LDFLAGS := -nostdlib -z max-page-size=0x1000 -T stage2/linker.ld

COM_CFLAGS := $(COMMON_CFLAGS) -Iboot/proto
COM_HELLO_SRC := com/hello/hello.c
COM_HELLO_ELF := build/INIT.COM.elf
COM_HELLO_BIN := build/INIT.COM
COM_CIUKEDIT_SRC := com/ciukedit/ciukedit.c
COM_CIUKEDIT_ELF := build/CIUKEDIT.COM.elf
COM_CIUKEDIT_BIN := build/CIUKEDIT.COM
COM_GFXSMOKE_SRC := com/gfxsmoke/gfxsmoke.c
COM_GFXSMOKE_ELF := build/GFXSMK.COM.elf
COM_GFXSMOKE_BIN := build/GFXSMK.COM
COM_DOSMODE13_SRC := com/dosmode13/dosmode13.c
COM_DOSMODE13_ELF := build/DOSMD13.COM.elf
COM_DOSMODE13_BIN := build/DOSMD13.COM
COM_FADEDEMO_SRC := com/fadedemo/fadedemo.c
COM_FADEDEMO_ELF := build/FADEDMO.COM.elf
COM_FADEDEMO_BIN := build/FADEDMO.COM
COM_DOSRUN_SMOKE_SRC := com/dosrun_smoke/ciuksmk.c
COM_DOSRUN_SMOKE_ELF := build/CIUKSMK.COM.elf
COM_DOSRUN_SMOKE_BIN := build/CIUKSMK.COM
COM_DOSRUN_MZ_SRC := com/dosrun_mz/ciukmz.c
COM_DOSRUN_MZ_ELF := build/CIUKMZ.EXE.elf
COM_DOSRUN_MZ_PAYLOAD := build/CIUKMZ.EXE.payload.bin
COM_DOSRUN_MZ_BIN := build/CIUKMZ.EXE
COM_M6_SMOKE_SRC := com/m6_smoke/ciukpm.c
COM_M6_SMOKE_ELF := build/CIUKPM.EXE.elf
COM_M6_SMOKE_PAYLOAD := build/CIUKPM.EXE.payload.bin
COM_M6_SMOKE_BIN := build/CIUKPM.EXE
COM_M6_DOS4GW_SMOKE_SRC := com/m6_dos4gw_smoke/ciuk4gw.c
COM_M6_DOS4GW_SMOKE_ELF := build/CIUK4GW.EXE.elf
COM_M6_DOS4GW_SMOKE_PAYLOAD := build/CIUK4GW.EXE.payload.bin
COM_M6_DOS4GW_SMOKE_BIN := build/CIUK4GW.EXE
COM_M6_DPMI_SMOKE_SRC := com/m6_dpmi_smoke/ciukdpm.c
COM_M6_DPMI_SMOKE_ELF := build/CIUKDPM.EXE.elf
COM_M6_DPMI_SMOKE_PAYLOAD := build/CIUKDPM.EXE.payload.bin
COM_M6_DPMI_SMOKE_BIN := build/CIUKDPM.EXE
COM_M6_DPMI_CALL_SMOKE_SRC := com/m6_dpmi_call_smoke/ciuk31.c
COM_M6_DPMI_CALL_SMOKE_ELF := build/CIUK31.EXE.elf
COM_M6_DPMI_CALL_SMOKE_PAYLOAD := build/CIUK31.EXE.payload.bin
COM_M6_DPMI_CALL_SMOKE_BIN := build/CIUK31.EXE
COM_M6_DPMI_BOOTSTRAP_SMOKE_SRC := com/m6_dpmi_bootstrap_smoke/ciuk306.c
COM_M6_DPMI_BOOTSTRAP_SMOKE_ELF := build/CIUK306.EXE.elf
COM_M6_DPMI_BOOTSTRAP_SMOKE_PAYLOAD := build/CIUK306.EXE.payload.bin
COM_M6_DPMI_BOOTSTRAP_SMOKE_BIN := build/CIUK306.EXE
COM_M6_DPMI_LDT_SMOKE_SRC := com/m6_dpmi_ldt_smoke/ciukldt.c
COM_M6_DPMI_LDT_SMOKE_ELF := build/CIUKLDT.EXE.elf
COM_M6_DPMI_LDT_SMOKE_PAYLOAD := build/CIUKLDT.EXE.payload.bin
COM_M6_DPMI_LDT_SMOKE_BIN := build/CIUKLDT.EXE
COM_M6_DPMI_MEM_SMOKE_SRC := com/m6_dpmi_mem_smoke/ciukmem.c
COM_M6_DPMI_MEM_SMOKE_ELF := build/CIUKMEM.EXE.elf
COM_M6_DPMI_MEM_SMOKE_PAYLOAD := build/CIUKMEM.EXE.payload.bin
COM_M6_DPMI_MEM_SMOKE_BIN := build/CIUKMEM.EXE
MKCIUKMZ_TOOL_SRC := tools/mkciukmz_exe.c
MKCIUKMZ_TOOL := build/tools/mkciukmz_exe
SPLASH_ASCII_SRC := misc/splashscreen.txt
SPLASH_GEN_C := build/generated/splash_data.c
SPLASH_GEN_OBJ := build/obj/stage2/splash_data.o
SPLASH_IMAGE_SRC ?= misc/CiukiOS_SplashScreen.png
SPLASH_IMAGE_GEN_C := build/generated/splash_image_data.c
SPLASH_IMAGE_GEN_OBJ := build/obj/stage2/splash_image_data.o
SPLASH_IMAGE_GEN_SCRIPT := scripts/generate_splash_image_c.sh

KERNEL_C_SRCS := $(shell find kernel/src -type f -name '*.c')
KERNEL_S_SRCS := $(shell find kernel/src -type f -name '*.S')
STAGE2_C_SRCS := $(shell find stage2/src -type f -name '*.c')
STAGE2_S_SRCS := $(shell find stage2/src -type f -name '*.S')

KERNEL_C_OBJS := $(patsubst kernel/src/%.c,build/obj/%.o,$(KERNEL_C_SRCS))
KERNEL_S_OBJS := $(patsubst kernel/src/%.S,build/obj/%.o,$(KERNEL_S_SRCS))
STAGE2_C_OBJS := $(patsubst stage2/src/%.c,build/obj/stage2/%.o,$(STAGE2_C_SRCS))
STAGE2_S_OBJS := $(patsubst stage2/src/%.S,build/obj/stage2/%.o,$(STAGE2_S_SRCS))

KERNEL_OBJS := $(KERNEL_C_OBJS) $(KERNEL_S_OBJS)
STAGE2_OBJS := $(STAGE2_C_OBJS) $(STAGE2_S_OBJS) $(SPLASH_GEN_OBJ) $(SPLASH_IMAGE_GEN_OBJ)

.DEFAULT_GOAL := all

all: build/kernel.elf build/stage2.elf $(COM_HELLO_BIN) $(COM_CIUKEDIT_BIN) $(COM_GFXSMOKE_BIN) $(COM_DOSMODE13_BIN) $(COM_FADEDEMO_BIN) $(COM_DOSRUN_SMOKE_BIN) $(COM_DOSRUN_MZ_BIN) $(COM_M6_SMOKE_BIN) $(COM_M6_DOS4GW_SMOKE_BIN) $(COM_M6_DPMI_SMOKE_BIN) $(COM_M6_DPMI_CALL_SMOKE_BIN) $(COM_M6_DPMI_BOOTSTRAP_SMOKE_BIN) $(COM_M6_DPMI_LDT_SMOKE_BIN) $(COM_M6_DPMI_MEM_SMOKE_BIN)

build/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld | build
	$(LD) $(KERNEL_LDFLAGS) -o $@ $(KERNEL_OBJS)

build/stage2.elf: $(STAGE2_OBJS) stage2/linker.ld | build
	$(LD) $(STAGE2_LDFLAGS) -o $@ $(STAGE2_OBJS)

build/obj/%.o: kernel/src/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

build/obj/%.o: kernel/src/%.S | build
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

build/obj/stage2/%.o: stage2/src/%.c | build
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CFLAGS) -c $< -o $@

build/obj/stage2/%.o: stage2/src/%.S | build
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(SPLASH_GEN_C): $(SPLASH_ASCII_SRC) | build
	@mkdir -p $(dir $@)
	xxd -i -n stage2_splash_ascii $< > $@

$(SPLASH_GEN_OBJ): $(SPLASH_GEN_C) | build
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CFLAGS) -c $< -o $@

$(SPLASH_IMAGE_GEN_C): $(SPLASH_IMAGE_GEN_SCRIPT) | build
	@mkdir -p $(dir $@)
	@if [ -f "$(SPLASH_IMAGE_SRC)" ]; then \
		"$(SPLASH_IMAGE_GEN_SCRIPT)" --input "$(SPLASH_IMAGE_SRC)" --output "$@"; \
	else \
		echo "[warn] splash image not found at $(SPLASH_IMAGE_SRC), generating empty asset"; \
		printf '%s\n' \
			'unsigned int stage2_splash_image_width = 0U;' \
			'unsigned int stage2_splash_image_height = 0U;' \
			'unsigned char stage2_splash_image_rgba[] = { 0x00 };' \
			'unsigned int stage2_splash_image_rgba_len = 0U;' > "$@"; \
	fi

$(SPLASH_IMAGE_GEN_OBJ): $(SPLASH_IMAGE_GEN_C) | build
	@mkdir -p $(dir $@)
	$(CC) $(STAGE2_CFLAGS) -c $< -o $@

$(COM_HELLO_BIN): $(COM_HELLO_SRC) com/hello/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_HELLO_SRC) -o build/obj/com/hello.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/hello/linker.ld -o $(COM_HELLO_ELF) build/obj/com/hello.o
	llvm-objcopy -O binary $(COM_HELLO_ELF) $(COM_HELLO_BIN)

$(COM_CIUKEDIT_BIN): $(COM_CIUKEDIT_SRC) com/ciukedit/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_CIUKEDIT_SRC) -o build/obj/com/ciukedit.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/ciukedit/linker.ld -o $(COM_CIUKEDIT_ELF) build/obj/com/ciukedit.o
	llvm-objcopy --set-section-flags .data=alloc,load,contents,data -O binary $(COM_CIUKEDIT_ELF) $(COM_CIUKEDIT_BIN)

$(COM_GFXSMOKE_BIN): $(COM_GFXSMOKE_SRC) com/gfxsmoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_GFXSMOKE_SRC) -o build/obj/com/gfxsmoke.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/gfxsmoke/linker.ld -o $(COM_GFXSMOKE_ELF) build/obj/com/gfxsmoke.o
	llvm-objcopy --set-section-flags .data=alloc,load,contents,data -O binary $(COM_GFXSMOKE_ELF) $(COM_GFXSMOKE_BIN)

$(COM_DOSMODE13_BIN): $(COM_DOSMODE13_SRC) com/dosmode13/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_DOSMODE13_SRC) -o build/obj/com/dosmode13.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/dosmode13/linker.ld -o $(COM_DOSMODE13_ELF) build/obj/com/dosmode13.o
	llvm-objcopy --set-section-flags .data=alloc,load,contents,data -O binary $(COM_DOSMODE13_ELF) $(COM_DOSMODE13_BIN)

$(COM_FADEDEMO_BIN): $(COM_FADEDEMO_SRC) com/fadedemo/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_FADEDEMO_SRC) -o build/obj/com/fadedemo.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/fadedemo/linker.ld -o $(COM_FADEDEMO_ELF) build/obj/com/fadedemo.o
	llvm-objcopy --set-section-flags .data=alloc,load,contents,data -O binary $(COM_FADEDEMO_ELF) $(COM_FADEDEMO_BIN)

$(COM_DOSRUN_SMOKE_BIN): $(COM_DOSRUN_SMOKE_SRC) com/dosrun_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_DOSRUN_SMOKE_SRC) -o build/obj/com/ciuksmk.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/dosrun_smoke/linker.ld -o $(COM_DOSRUN_SMOKE_ELF) build/obj/com/ciuksmk.o
	llvm-objcopy -O binary $(COM_DOSRUN_SMOKE_ELF) $(COM_DOSRUN_SMOKE_BIN)

$(MKCIUKMZ_TOOL): $(MKCIUKMZ_TOOL_SRC) | build
	@mkdir -p build/tools
	clang -O2 -std=c11 -Wall -Wextra -o $@ $<

$(COM_DOSRUN_MZ_PAYLOAD): $(COM_DOSRUN_MZ_SRC) com/dosrun_mz/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_DOSRUN_MZ_SRC) -o build/obj/com/ciukmz.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/dosrun_mz/linker.ld -o $(COM_DOSRUN_MZ_ELF) build/obj/com/ciukmz.o
	llvm-objcopy -O binary $(COM_DOSRUN_MZ_ELF) $(COM_DOSRUN_MZ_PAYLOAD)

$(COM_DOSRUN_MZ_BIN): $(COM_DOSRUN_MZ_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_DOSRUN_MZ_PAYLOAD) $@

$(COM_M6_SMOKE_PAYLOAD): $(COM_M6_SMOKE_SRC) com/m6_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_SMOKE_SRC) -o build/obj/com/ciukpm.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_smoke/linker.ld -o $(COM_M6_SMOKE_ELF) build/obj/com/ciukpm.o
	llvm-objcopy -O binary $(COM_M6_SMOKE_ELF) $(COM_M6_SMOKE_PAYLOAD)

$(COM_M6_SMOKE_BIN): $(COM_M6_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_SMOKE_PAYLOAD) $@

$(COM_M6_DOS4GW_SMOKE_PAYLOAD): $(COM_M6_DOS4GW_SMOKE_SRC) com/m6_dos4gw_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DOS4GW_SMOKE_SRC) -o build/obj/com/ciuk4gw.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dos4gw_smoke/linker.ld -o $(COM_M6_DOS4GW_SMOKE_ELF) build/obj/com/ciuk4gw.o
	llvm-objcopy -O binary $(COM_M6_DOS4GW_SMOKE_ELF) $(COM_M6_DOS4GW_SMOKE_PAYLOAD)

$(COM_M6_DOS4GW_SMOKE_BIN): $(COM_M6_DOS4GW_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DOS4GW_SMOKE_PAYLOAD) $@

$(COM_M6_DPMI_SMOKE_PAYLOAD): $(COM_M6_DPMI_SMOKE_SRC) com/m6_dpmi_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DPMI_SMOKE_SRC) -o build/obj/com/ciukdpm.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_smoke/linker.ld -o $(COM_M6_DPMI_SMOKE_ELF) build/obj/com/ciukdpm.o
	llvm-objcopy -O binary $(COM_M6_DPMI_SMOKE_ELF) $(COM_M6_DPMI_SMOKE_PAYLOAD)

$(COM_M6_DPMI_SMOKE_BIN): $(COM_M6_DPMI_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DPMI_SMOKE_PAYLOAD) $@

$(COM_M6_DPMI_CALL_SMOKE_PAYLOAD): $(COM_M6_DPMI_CALL_SMOKE_SRC) com/m6_dpmi_call_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DPMI_CALL_SMOKE_SRC) -o build/obj/com/ciuk31.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_call_smoke/linker.ld -o $(COM_M6_DPMI_CALL_SMOKE_ELF) build/obj/com/ciuk31.o
	llvm-objcopy -O binary $(COM_M6_DPMI_CALL_SMOKE_ELF) $(COM_M6_DPMI_CALL_SMOKE_PAYLOAD)

$(COM_M6_DPMI_CALL_SMOKE_BIN): $(COM_M6_DPMI_CALL_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DPMI_CALL_SMOKE_PAYLOAD) $@

$(COM_M6_DPMI_BOOTSTRAP_SMOKE_PAYLOAD): $(COM_M6_DPMI_BOOTSTRAP_SMOKE_SRC) com/m6_dpmi_bootstrap_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DPMI_BOOTSTRAP_SMOKE_SRC) -o build/obj/com/ciuk306.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_bootstrap_smoke/linker.ld -o $(COM_M6_DPMI_BOOTSTRAP_SMOKE_ELF) build/obj/com/ciuk306.o
	llvm-objcopy -O binary $(COM_M6_DPMI_BOOTSTRAP_SMOKE_ELF) $(COM_M6_DPMI_BOOTSTRAP_SMOKE_PAYLOAD)

$(COM_M6_DPMI_BOOTSTRAP_SMOKE_BIN): $(COM_M6_DPMI_BOOTSTRAP_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DPMI_BOOTSTRAP_SMOKE_PAYLOAD) $@

$(COM_M6_DPMI_LDT_SMOKE_PAYLOAD): $(COM_M6_DPMI_LDT_SMOKE_SRC) com/m6_dpmi_ldt_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DPMI_LDT_SMOKE_SRC) -o build/obj/com/ciukldt.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_ldt_smoke/linker.ld -o $(COM_M6_DPMI_LDT_SMOKE_ELF) build/obj/com/ciukldt.o
	llvm-objcopy -O binary $(COM_M6_DPMI_LDT_SMOKE_ELF) $(COM_M6_DPMI_LDT_SMOKE_PAYLOAD)

$(COM_M6_DPMI_LDT_SMOKE_BIN): $(COM_M6_DPMI_LDT_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DPMI_LDT_SMOKE_PAYLOAD) $@

$(COM_M6_DPMI_MEM_SMOKE_PAYLOAD): $(COM_M6_DPMI_MEM_SMOKE_SRC) com/m6_dpmi_mem_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_M6_DPMI_MEM_SMOKE_SRC) -o build/obj/com/ciukmem.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/m6_dpmi_mem_smoke/linker.ld -o $(COM_M6_DPMI_MEM_SMOKE_ELF) build/obj/com/ciukmem.o
	llvm-objcopy -O binary $(COM_M6_DPMI_MEM_SMOKE_ELF) $(COM_M6_DPMI_MEM_SMOKE_PAYLOAD)

$(COM_M6_DPMI_MEM_SMOKE_BIN): $(COM_M6_DPMI_MEM_SMOKE_PAYLOAD) $(MKCIUKMZ_TOOL)
	$(MKCIUKMZ_TOOL) $(COM_M6_DPMI_MEM_SMOKE_PAYLOAD) $@

build:
	@mkdir -p build
	@mkdir -p build/obj

clean:
	rm -rf build

re: clean all

test-stage2:
	./scripts/test_stage2_boot.sh

test-fallback:
	./scripts/test_kernel_fallback_boot.sh

test-video-mode:
	./scripts/test_video_mode_pipeline.sh

test-video-1024:
	bash ./scripts/test_video_1024_compat.sh

test-video-backbuf:
	bash ./scripts/test_video_backbuf_policy.sh

test-vmode-persistence:
	bash ./scripts/test_vmode_persistence_reboot.sh

test-m6-pmode:
	bash ./scripts/test_m6_pmode_contract.sh

test-m6-transition-v2:
	bash ./scripts/test_m6_transition_contract_v2.sh

test-m6-smoke:
	bash ./scripts/test_m6_dos_program.sh

test-m6-dos4gw-smoke:
	bash ./scripts/test_m6_dos4gw_smoke.sh

test-m6-dpmi-ldt-smoke:
	bash ./scripts/test_m6_dpmi_ldt_smoke.sh

test-m6-dpmi-mem-smoke:
	bash ./scripts/test_m6_dpmi_mem_smoke.sh

test-m6-dpmi-smoke:
	bash ./scripts/test_m6_dpmi_smoke.sh

test-m6-dpmi-call-smoke:
	bash ./scripts/test_m6_dpmi_call_smoke.sh

test-m6-dpmi-bootstrap-smoke:
	bash ./scripts/test_m6_dpmi_bootstrap_smoke.sh

test-dosrun-simple:
	bash ./scripts/test_dosrun_simple_program.sh

test-ciukedit-smoke:
	bash ./scripts/test_ciukedit_smoke.sh

test-dosrun-mz:
	bash ./scripts/test_dosrun_mz_simple.sh

test-fat-compat:
	./scripts/test_fat_compat.sh

test-fat32-progress:
	bash ./scripts/test_fat32_progress.sh

test-fat32-edge:
	bash ./scripts/test_fat32_edge_semantics.sh

test-int21:
	./scripts/test_int21_priority_a.sh

test-mz-regression:
	bash ./scripts/test_mz_regression.sh

test-mz-corpus:
	bash ./scripts/test_mz_runtime_corpus.sh

test-phase2:
	bash ./scripts/test_phase2_closure.sh

test-freedos-pipeline:
	./scripts/validate_freedos_pipeline.sh

check-int21-matrix:
	./scripts/check_int21_matrix.sh

test-gui-desktop:
	./scripts/test_gui_desktop.sh

test-startup-chain:
	bash ./scripts/test_startup_chain.sh

test-video-ui-v2:
	bash ./scripts/test_video_ui_regression_v2.sh

test-video-policy-matrix:
	bash ./scripts/test_video_policy_matrix.sh

test-opengem:
	./scripts/test_opengem_integration.sh

test-doom-target-packaging:
	bash ./scripts/test_doom_target_packaging.sh

test-vga13-baseline:
	bash ./scripts/test_vga13_baseline.sh

test-doom-boot-harness:
	bash ./scripts/test_doom_boot_harness.sh

test-boot: test-stage2 test-fallback

ci: test-boot

run:
	./run_ciukios.sh

run-nofreedos:
	CIUKIOS_INCLUDE_FREEDOS=0 ./run_ciukios.sh

freedos-import:
	@if [ -z "$(FREEDOS_SRC)" ]; then \
		echo "Usage: make freedos-import FREEDOS_SRC=/path/to/freedos/files"; \
		exit 1; \
	fi
	./scripts/import_freedos.sh --source "$(FREEDOS_SRC)"

freecom-sync:
	./scripts/sync_freecom_repo.sh

freedos-sync-upstreams:
	bash ./scripts/sync_freedos_upstreams.sh

freedos-runtime-manifest:
	bash ./scripts/generate_freedos_runtime_manifest.sh

freecom-build:
	./scripts/build_freecom.sh

.PHONY: all clean re test-stage2 test-fallback test-video-mode test-video-1024 test-video-backbuf test-vmode-persistence test-m6-pmode test-m6-transition-v2 test-m6-smoke test-m6-dos4gw-smoke test-m6-dpmi-smoke test-m6-dpmi-call-smoke test-m6-dpmi-bootstrap-smoke test-m6-dpmi-ldt-smoke test-m6-dpmi-mem-smoke test-vga13-baseline test-doom-boot-harness test-dosrun-simple test-ciukedit-smoke test-dosrun-mz test-fat-compat test-fat32-progress test-int21 test-mz-regression test-mz-corpus test-phase2 test-freedos-pipeline check-int21-matrix test-gui-desktop test-video-ui-v2 test-video-policy-matrix test-opengem test-doom-target-packaging test-boot ci run run-nofreedos freedos-import freecom-sync freecom-build freedos-sync-upstreams freedos-runtime-manifest
