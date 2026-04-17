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
COM_DOSRUN_SMOKE_SRC := com/dosrun_smoke/ciuksmk.c
COM_DOSRUN_SMOKE_ELF := build/CIUKSMK.COM.elf
COM_DOSRUN_SMOKE_BIN := build/CIUKSMK.COM
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

all: build/kernel.elf build/stage2.elf $(COM_HELLO_BIN) $(COM_DOSRUN_SMOKE_BIN)

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

$(COM_DOSRUN_SMOKE_BIN): $(COM_DOSRUN_SMOKE_SRC) com/dosrun_smoke/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_DOSRUN_SMOKE_SRC) -o build/obj/com/ciuksmk.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/dosrun_smoke/linker.ld -o $(COM_DOSRUN_SMOKE_ELF) build/obj/com/ciuksmk.o
	llvm-objcopy -O binary $(COM_DOSRUN_SMOKE_ELF) $(COM_DOSRUN_SMOKE_BIN)

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
.PHONY: all clean re test-stage2 test-fallback test-video-mode test-video-1024 test-video-backbuf test-m6-pmode test-dosrun-simple test-fat-compat test-fat32-progress test-int21 test-mz-regression test-mz-corpus test-phase2 test-freedos-pipeline check-int21-matrix test-gui-desktop test-video-ui-v2 test-opengem test-boot ci run run-nofreedos freedos-import freecom-sync freecom-build freedos-sync-upstreams freedos-runtime-manifest
.PHONY: all clean re test-stage2 test-fallback test-video-mode test-video-1024 test-video-backbuf test-vmode-persistence test-m6-pmode test-dosrun-simple test-fat-compat test-fat32-progress test-int21 test-mz-regression test-mz-corpus test-phase2 test-freedos-pipeline check-int21-matrix test-gui-desktop test-video-ui-v2 test-opengem test-boot ci run run-nofreedos freedos-import freecom-sync freecom-build freedos-sync-upstreams freedos-runtime-manifest

test-m6-pmode:
	bash ./scripts/test_m6_pmode_contract.sh

test-dosrun-simple:
	bash ./scripts/test_dosrun_simple_program.sh

test-fat-compat:
	./scripts/test_fat_compat.sh

test-fat32-progress:
	bash ./scripts/test_fat32_progress.sh

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

test-video-ui-v2:
	bash ./scripts/test_video_ui_regression_v2.sh

test-opengem:
	./scripts/test_opengem_integration.sh

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

.PHONY: all clean re test-stage2 test-fallback test-video-mode test-video-1024 test-video-backbuf test-m6-pmode test-fat-compat test-fat32-progress test-int21 test-mz-regression test-mz-corpus test-phase2 test-freedos-pipeline check-int21-matrix test-gui-desktop test-video-ui-v2 test-opengem test-boot ci run run-nofreedos freedos-import freecom-sync freecom-build freedos-sync-upstreams freedos-runtime-manifest
