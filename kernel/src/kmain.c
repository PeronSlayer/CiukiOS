#include "bootinfo.h"
#include "serial.h"

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void kmain(boot_info_t *boot_info) {
    serial_init();
    serial_write("\n[ CiukiOS ] kernel started\n");

    if (!boot_info) {
        serial_write("[ panic ] null boot_info\n");
        halt_forever();
    }

    if (boot_info->magic != BOOTINFO_MAGIC) {
        serial_write("[ panic ] invalid boot_info magic: 0x");
        serial_write_hex64(boot_info->magic);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] boot_info is valid\n");

    serial_write("[ info ] memory_map_ptr: 0x");
    serial_write_hex64(boot_info->memory_map_ptr);
    serial_write("\n");

    serial_write("[ info ] memory_map_size: 0x");
    serial_write_hex64(boot_info->memory_map_size);
    serial_write("\n");

    serial_write("[ info ] memory_map_desc_size: 0x");
    serial_write_hex64(boot_info->memory_map_descriptor_size);
    serial_write("\n");

    serial_write("[ info ] kernel_phys_base: 0x");
    serial_write_hex64(boot_info->kernel_phys_base);
    serial_write("\n");

    serial_write("[ info ] kernel_phys_size: 0x");
    serial_write_hex64(boot_info->kernel_phys_size);
    serial_write("\n");

    serial_write("[ ok ] checks passed from custom UEFI loader\n");

    halt_forever();
}
