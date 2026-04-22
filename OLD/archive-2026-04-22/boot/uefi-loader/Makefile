CC := clang
LD := ld.bfd
OBJCOPY := objcopy

EFI_INC := /usr/include/efi
EFI_ARCH_INC := /usr/include/efi/x86_64
EFI_LIB := /usr/lib
EFI_GNUEFI_LIB := /usr/lib
EFI_CRT := $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS := $(EFI_LIB)/elf_x86_64_efi.lds

CFLAGS := -I$(EFI_INC) \
          -I$(EFI_ARCH_INC) \
          -fpic \
          -ffreestanding \
          -fno-stack-protector \
          -fshort-wchar \
          -mno-red-zone \
          -Wall \
          -Wextra

ASFLAGS := -fpic -ffreestanding -mno-red-zone

LDFLAGS := -nostdlib \
           -znocombreloc \
           -T $(EFI_LDS) \
           -shared \
           -Bsymbolic

LIBS := -L$(EFI_GNUEFI_LIB) -L$(EFI_LIB) -lefi -lgnuefi

all: build/BOOTX64.EFI

build:
	mkdir -p build

build/loader.o: loader.c elf64.h | build
	$(CC) $(CFLAGS) -c loader.c -o $@

build/handoff.o: handoff.S | build
	$(CC) $(ASFLAGS) -c handoff.S -o $@

build/loader.so: build/loader.o build/handoff.o
	$(LD) $(LDFLAGS) $(EFI_CRT) build/loader.o build/handoff.o -o $@ $(LIBS)

build/BOOTX64.EFI: build/loader.so
	$(OBJCOPY) \
		-j .text \
		-j .sdata \
		-j .data \
		-j .dynamic \
		-j .dynsym \
		-j .rela \
		-j .rel \
		-j .reloc \
		-j .rodata \
		-O efi-app-x86_64 \
		$< $@

clean:
	rm -rf build

.PHONY: all clean
