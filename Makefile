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

KERNEL_C_SRCS := $(shell find kernel/src -type f -name '*.c')
KERNEL_S_SRCS := $(shell find kernel/src -type f -name '*.S')
STAGE2_C_SRCS := $(shell find stage2/src -type f -name '*.c')
STAGE2_S_SRCS := $(shell find stage2/src -type f -name '*.S')

KERNEL_C_OBJS := $(patsubst kernel/src/%.c,build/obj/%.o,$(KERNEL_C_SRCS))
KERNEL_S_OBJS := $(patsubst kernel/src/%.S,build/obj/%.o,$(KERNEL_S_SRCS))
STAGE2_C_OBJS := $(patsubst stage2/src/%.c,build/obj/stage2/%.o,$(STAGE2_C_SRCS))
STAGE2_S_OBJS := $(patsubst stage2/src/%.S,build/obj/stage2/%.o,$(STAGE2_S_SRCS))

KERNEL_OBJS := $(KERNEL_C_OBJS) $(KERNEL_S_OBJS)
STAGE2_OBJS := $(STAGE2_C_OBJS) $(STAGE2_S_OBJS)

.DEFAULT_GOAL := all

all: build/kernel.elf build/stage2.elf $(COM_HELLO_BIN)

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

$(COM_HELLO_BIN): $(COM_HELLO_SRC) com/hello/linker.ld boot/proto/services.h | build
	@mkdir -p build/obj/com
	$(CC) $(COM_CFLAGS) -c $(COM_HELLO_SRC) -o build/obj/com/hello.o
	$(LD) -nostdlib -z max-page-size=0x1000 -T com/hello/linker.ld -o $(COM_HELLO_ELF) build/obj/com/hello.o
	llvm-objcopy -O binary $(COM_HELLO_ELF) $(COM_HELLO_BIN)

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

test-boot: test-stage2 test-fallback

ci: test-boot

.PHONY: all clean re test-stage2 test-fallback test-boot ci
