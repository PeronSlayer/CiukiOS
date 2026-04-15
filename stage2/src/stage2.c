#include "serial.h"
#include "bootinfo.h"
#include "handoff.h"

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void stage2_main(boot_info_t *boot_info, handoff_v0_t *handoff) {
    serial_init();
    serial_write("\n[ stage2 ] scaffolding started\n");

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

    if (!handoff) {
        serial_write("[ panic ] null handoff\n");
        halt_forever();
    }

    if (handoff->magic != HANDOFF_V0_MAGIC) {
        serial_write("[ panic ] invalid handoff magic: 0x");
        serial_write_hex64(handoff->magic);
        serial_write("\n");
        halt_forever();
    }

    if (handoff->version != HANDOFF_V0_VERSION) {
        serial_write("[ panic ] invalid handoff version: 0x");
        serial_write_hex64(handoff->version);
        serial_write("\n");
        halt_forever();
    }

    serial_write("[ ok ] handoff v0 is valid\n");
    serial_write("[ info ] stage2_load_addr: 0x");
    serial_write_hex64(handoff->stage2_load_addr);
    serial_write("\n");
    serial_write("[ info ] stage2_size: 0x");
    serial_write_hex64(handoff->stage2_size);
    serial_write("\n");

    serial_write("[ stage2 ] next step: handoff to DOS-like runtime\n");
    halt_forever();
}
